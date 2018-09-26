#include "vm.h"
#include "opcode.h"
#include "import.h"
#include "modules.h"
#include "core.h"
#include "util.h"
#include "sys.h"
#include "blang.h"

#include "debug/disassemble.h"

#include "parse/parser.h"
#include "parse/ast.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <float.h>
#include <math.h>
#include <limits.h>
#include <string.h>

static bool unwindStack(VM *vm);

static void reset(VM *vm) {
	vm->sp = vm->stack;
	vm->frameCount = 0;
	vm->exception = NULL;
}

void initVM(VM *vm) {
	vm->importpaths = NULL;

	vm->currCompiler = NULL;
	vm->ctor = NULL;

	vm->exception = NULL;
	sbuf_create(&vm->stacktrace);

	vm->stack  = malloc(sizeof(Value) * STACK_SZ);
	vm->frames = malloc(sizeof(Frame) * FRAME_SZ);
	vm->stackend = vm->stack + STACK_SZ;

	vm->clsClass  = NULL;
	vm->objClass  = NULL;
	vm->strClass  = NULL;
	vm->boolClass = NULL;
	vm->lstClass  = NULL;
	vm->numClass  = NULL;
	vm->funClass  = NULL;
	vm->modClass  = NULL;
	vm->nullClass = NULL;
	vm->excClass  = NULL;

	reset(vm);

	initHashTable(&vm->modules);
	initHashTable(&vm->strings);

	vm->module = NULL;

	vm->nextGC = INIT_GC;
	vm->objects = NULL;
	vm->disableGC = false;

	vm->allocated = 0;
	vm->reachedStack = NULL;
	vm->reachedCapacity = 0;
	vm->reachedCount = 0;

	vm->ctor = copyString(vm, CTOR_STR, strlen(CTOR_STR));

	initCoreLibrary(vm);

	// This is called after initCoreLibrary in order to correctly assign the
	// List class to the object since it's created during the initialization
	vm->importpaths = newList(vm, 8);
}

void push(VM *vm, Value v) {
	*vm->sp++ = v;
}

Value pop(VM *vm) {
	return *--vm->sp;
}

static ObjClass *getClass(VM *vm, Value v) {
	if(IS_OBJ(v)) {
		return AS_OBJ(v)->cls;
	} else if(IS_NUM(v)) {
		return vm->numClass;
	} else if(IS_BOOL(v)) {
		return vm->boolClass;
	} else {
		return vm->nullClass;
	}
}

static bool isInstance(VM *vm, Value i, ObjClass *cls) {
	for(ObjClass *c = getClass(vm, i); c != NULL; c = c->superCls) {
		if(c == cls) {
			return true;
		}
	}
	return false;
}

static bool callFunction(VM *vm, ObjFunction *func, uint8_t argc) {
	if(func->argsCount != argc) {
		blRise(vm, "TypeException", "Function `%s` expected %d args, but "
		    "instead %d supplied.", func->name->data, func->argsCount, argc);
		return false;
	}

	if(vm->frameCount == FRAME_SZ) {
		blRise(vm, "StackOverflowException", NULL);
		return false;
	}

	Frame *callFrame = &vm->frames[vm->frameCount++];
	callFrame->fn = func;
	callFrame->ip = func->chunk.code;
	callFrame->stack = vm->sp - (argc + 1);
	callFrame->handlerc = 0;

	vm->module = func->module;

	return true;
}

static bool callNative(VM *vm, ObjNative *native, uint8_t argc) {
	if(native->argsCount != argc) {
		blRise(vm, "TypeException", "Native function `%s` expexted %d args, "
		        "but instead %d supplied.", native->name->data, native->argsCount, argc);
		return false;
	}

	Value ret;
	if(!native->fn(vm, vm->sp - (argc + 1), &ret)) {
		blRise(vm, "Exception", "Failed to call native %s().", native->name->data);
		return false;
	}
	vm->sp -= argc + 1;
	push(vm, ret);

	return vm->exception == NULL;
}

static bool callValue(VM *vm, Value callee, uint8_t argc) {
	if(IS_OBJ(callee)) {
		switch(OBJ_TYPE(callee)) {
		case OBJ_FUNCTION:
			return callFunction(vm, AS_FUNC(callee), argc);
		case OBJ_NATIVE:
			return callNative(vm, AS_NATIVE(callee), argc);
		case OBJ_BOUND_METHOD: {
			ObjBoundMethod *m = AS_BOUND_METHOD(callee);
			vm->sp[-argc - 1] = OBJ_VAL(m->bound);
			return m->method->type == OBJ_FUNCTION ?
			        callFunction(vm, (ObjFunction*)m->method, argc) :
			        callNative(vm, (ObjNative*)m->method, argc);
		}
		case OBJ_CLASS: {
			ObjClass *cls = AS_CLASS(callee);
			vm->sp[-argc - 1] = OBJ_VAL(newInstance(vm, cls));

			Value ctor;
			if(hashTableGet(&cls->methods, vm->ctor, &ctor)) {
				return callFunction(vm, AS_FUNC(ctor), argc);
			} else if(argc != 0) {
				blRise(vm, "TypeException", "Function %s.new() Expected 0 "
				    "args, but instead `%d` supplied.", cls->name->data, argc);
				return false;
			}

			return true;
		}
		default: break;
		}
	}

	ObjClass *cls = getClass(vm, callee);
	blRise(vm, "TypeException", "Object %s is not a callable.", cls->name->data);
	return false;
}

static void createClass(VM *vm, ObjString *name, ObjClass *superCls) {
	ObjClass *cls = newClass(vm, name, superCls);

	if(superCls != NULL) {
		hashTableMerge(&cls->methods, &superCls->methods);
	}

	push(vm, OBJ_VAL(cls));
}

static bool invokeMethod(VM *vm, ObjClass *cls, ObjString *name, uint8_t argc) {
	Value method;
	if(!hashTableGet(&cls->methods, name, &method)) {
		blRise(vm, "MethodException", "Method %s.%s() doesn't "
			                "exists", cls->name->data, name->data);
		return false;
	}

	return callValue(vm, method, argc);
}

static bool invokeFromValue(VM *vm, ObjString *name, uint8_t argc) {
	Value val = peekn(vm, argc);
	if(IS_OBJ(val)) {
		switch(OBJ_TYPE(val)) {
		case OBJ_INST: {
			ObjInstance *inst = AS_INSTANCE(val);

			// Check if field shadows a method
			Value f;
			if(hashTableGet(&inst->fields, name, &f)) {
				if(!IS_OBJ(f) && (IS_FUNC(f) || IS_NATIVE(f)
								|| IS_CLASS(f) || IS_BOUND_METHOD(f))) {
					return callValue(vm, f, argc);
				}
			}

			return invokeMethod(vm, inst->base.cls, name, argc);
		}
		case OBJ_MODULE: {
			ObjModule *mod = AS_MODULE(val);

			Value func;
			// check if method shadows a function in the module
			if(hashTableGet(&vm->modClass->methods, name, &func)) {
				return callValue(vm, func, argc);
			}

			if(!hashTableGet(&mod->globals, name, &func)) {
				blRise(vm, "NameException", "Name `%s` is not defined "
					        "in module %s.", name->data, mod->name->data);
				return false;
			}

			return callValue(vm, func, argc);
		}
		default: {
			Obj *o = AS_OBJ(val);
			return invokeMethod(vm, o->cls, name, argc);
		}
		}
	}

	// if builtin type get the class and try to invoke
	ObjClass *cls = getClass(vm, val);
	return invokeMethod(vm, cls, name, argc);
}

bool getFieldFromValue(VM *vm, Value val, ObjString *name) {
	if(IS_OBJ(val)) {
		switch(OBJ_TYPE(val)) {
		case OBJ_INST: {
			ObjInstance *inst = AS_INSTANCE(val);

			Value v;
			if(!hashTableGet(&inst->fields, name, &v)) {
				//if we didnt find a field try to return bound method
				if(!hashTableGet(&inst->base.cls->methods, name, &v)) {
					blRise(vm, "FieldException", "Object %s doesn't have "
						"field `%s`.", inst->base.cls->name->data, name->data);
					return false;
				}

				push(vm, OBJ_VAL(newBoundMethod(vm, val, AS_OBJ(v))));
				return true;
			}

			push(vm, v);
			return true;
		}
		case OBJ_MODULE: {
			ObjModule *mod = AS_MODULE(val);

			Value v;
			if(!hashTableGet(&mod->globals, name, &v)) {
				//if we didnt find a global name try to return bound method
				if(!hashTableGet(&mod->base.cls->methods, name, &v)) {
					blRise(vm, "NameException", "Name `%s` is not "
						"defined in module %s", name->data, mod->name->data);
					return false;
				}

				push(vm, OBJ_VAL(newBoundMethod(vm, val, AS_OBJ(v))));
				return true;
			}

			push(vm, v);
			return true;
		}
		default: break;
		}
	}

	Value v;
	ObjClass *cls = getClass(vm, val);

	if(!hashTableGet(&cls->methods, name, &v)) {
		blRise(vm, "FieldException", "Object %s doesn't have "
			"field `%s`.", cls->name->data, name->data);
		return false;
	}

	push(vm, OBJ_VAL(newBoundMethod(vm, val, AS_OBJ(v))));
	return true;
}

bool setFieldOfValue(VM *vm, Value val, ObjString *name, Value s) {
	if(IS_OBJ(val)) {
		switch(OBJ_TYPE(val)) {
		case OBJ_INST: {
			ObjInstance *inst = AS_INSTANCE(val);
			hashTablePut(&inst->fields, name, s);
			return true;
		}
		case OBJ_MODULE: {
			ObjModule *mod = AS_MODULE(val);
			hashTablePut(&mod->globals, name, s);
			return true;
		}
		default: break;
		}
	}

	ObjClass *cls = getClass(vm, val);
	blRise(vm, "FieldException", "Object %s doesn't "
	        "have field `%s`.", cls->name->data, name->data);
	return false;
}

static bool isValTrue(Value val) {
	return ((IS_BOOL(val) && AS_BOOL(val))
	      ||(IS_NUM(val) && AS_NUM(val) != 0)
	      ||(IS_STRING(val) && AS_STRING(val)->length != 0)
	      ||(IS_OBJ(val) && !IS_STRING(val)));
}

static ObjString* stringConcatenate(VM *vm, ObjString *s1, ObjString *s2) {
	size_t length = s1->length + s2->length;
	char *data = ALLOC(vm, length + 1);
	memcpy(data, s1->data, s1->length);
	memcpy(data + s1->length, s2->data, s2->length);
	data[length] = '\0';
	return newStringFromBuf(vm, data, length);
}

static bool runEval(VM *vm) {
	Frame *frame = &vm->frames[vm->frameCount - 1];

	#define NEXT_CODE()  (*frame->ip++)
	#define NEXT_SHORT() (frame->ip += 2, ((uint16_t) frame->ip[-2] << 8) | frame->ip[-1])

	#define GET_CONST()  (frame->fn->chunk.consts.arr[NEXT_CODE()])
	#define GET_STRING() (AS_STRING(GET_CONST()))

	#define BINARY(type, op) do { \
		if(!IS_NUM(peek(vm)) || !IS_NUM(peek2(vm))) { \
			blRise(vm, "TypeException", "Operands of `%s` must be numbers.", #op); \
			UNWIND_STACK(vm); \
		} \
		double b = AS_NUM(pop(vm)); \
		double a = AS_NUM(pop(vm)); \
		push(vm, type(a op b)); \
	} while(0)

	#define UNWIND_STACK(vm) do { \
		if(!unwindStack(vm)) { \
			return false; \
		} \
		frame = &vm->frames[vm->frameCount - 1]; \
		DISPATCH(); \
	} while(0)

	#ifdef DBG_PRINT_EXEC
		#define PRINT_DBG_STACK() \
			printf("     "); \
			for(Value *v = vm->stack; v < vm->sp; v++) { \
				printf("["); \
				printValue(*v); \
				printf("]"); \
			} \
			printf("$\n"); \
			disassembleIstr(&frame->fn->chunk, (size_t) (frame-> ip - frame->fn->chunk.code));
	#else
		#define PRINT_DBG_STACK()
	#endif

	#ifdef USE_COMPUTED_GOTOS
		//import jumptable
		#include "opcode_jmp_table.h"

		#define TARGET(op) \
			TARGET_##op \

		#define DISPATCH() do { \
			PRINT_DBG_STACK() \
			goto *opJmpTable[(op = NEXT_CODE())]; \
		} while(0)

		#define CASE(op) DISPATCH();

	#else

		#define TARGET(op) case op
		#define DISPATCH() continue
		#define CASE(op) switch((op = NEXT_CODE()))

	#endif

	// Eval loop
	for(;;) {

#ifndef USE_COMPUTED_GOTOS
	PRINT_DBG_STACK()
#endif

	uint8_t op;
	CASE(op) {

	TARGET(OP_ADD): {
		if(IS_NUM(peek(vm)) && IS_NUM(peek2(vm))) {
			double b = AS_NUM(pop(vm));
			double a = AS_NUM(pop(vm));
			push(vm, NUM_VAL(a + b));
			DISPATCH();
		} else if(IS_STRING(peek(vm)) && IS_STRING(peek2(vm))) {
			ObjString *conc = stringConcatenate(vm, AS_STRING(peek2(vm)),
			                                        AS_STRING(peek(vm)));

			pop(vm);
			pop(vm);

			push(vm, OBJ_VAL(conc));
			DISPATCH();
		}
		blRise(vm, "TypeException", "Operands of + must be two numbers or strings.");
		UNWIND_STACK(vm);
	}
	TARGET(OP_MOD): {
		if(!IS_NUM(peek(vm)) || !IS_NUM(peek2(vm))) {
			blRise(vm, "TypeException", "Operands of % must be numbers.");
			UNWIND_STACK(vm);
		}
		double b = AS_NUM(pop(vm));
		double a = AS_NUM(pop(vm));

		if(b == 0) {
			blRise(vm, "DivisionByZeroException", "Modulo by zero.");
			UNWIND_STACK(vm);
		}

		push(vm, NUM_VAL(fmod(a, b)));
		DISPATCH();
	}
	TARGET(OP_DIV): {
		if(!IS_NUM(peek(vm)) || !IS_NUM(peek2(vm))) {
			blRise(vm, "TypeException", "Operands of / must be numbers.");
			UNWIND_STACK(vm);
		}
		double b = AS_NUM(pop(vm));
		double a = AS_NUM(pop(vm));

		if(b == 0) {
			blRise(vm, "DivisionByZeroException", "Division by zero.");
			UNWIND_STACK(vm);
		}

		push(vm, NUM_VAL(a / b));
		DISPATCH();
	}
	TARGET(OP_SUB): BINARY(NUM_VAL, -);   DISPATCH();
	TARGET(OP_MUL): BINARY(NUM_VAL, *);   DISPATCH();
	TARGET(OP_LT):  BINARY(BOOL_VAL, <);  DISPATCH();
	TARGET(OP_LE):  BINARY(BOOL_VAL, <=); DISPATCH();
	TARGET(OP_GT):  BINARY(BOOL_VAL, >);  DISPATCH();
	TARGET(OP_GE):  BINARY(BOOL_VAL, >=); DISPATCH();
	TARGET(OP_EQ): {
		Value b = pop(vm);
		Value a = pop(vm);
		push(vm, BOOL_VAL(valueEquals(a, b)));
		DISPATCH();
	}
	TARGET(OP_NEQ): {
		Value b = pop(vm);
		Value a = pop(vm);
		push(vm, BOOL_VAL(!valueEquals(a, b)));
		DISPATCH();
	}
	TARGET(OP_IS): {
		Value b = pop(vm);
		Value a = pop(vm);

		if(!IS_CLASS(b)) {
			blRise(vm, "TypeException", "Right operand of `is` must be a class.");
			UNWIND_STACK(vm);
		}

		push(vm, BOOL_VAL(isInstance(vm, a, AS_CLASS(b))));
		DISPATCH();
	}
	TARGET(OP_NEG):
		if(!IS_NUM(peek(vm))) {
			blRise(vm, "TypeException", "Operand to `-` must be a number.");
			UNWIND_STACK(vm);
		}
		push(vm, NUM_VAL(-AS_NUM(pop(vm))));
		DISPATCH();
	TARGET(OP_NOT):
		push(vm, BOOL_VAL(!isValTrue(pop(vm))));
		DISPATCH();
	TARGET(OP_GET_FIELD): {
		Value v = pop(vm);
		if(!getFieldFromValue(vm, v, GET_STRING())) {
			UNWIND_STACK(vm);
		}
		DISPATCH();
	}
	TARGET(OP_SET_FIELD): {
		Value v = pop(vm);
		if(!setFieldOfValue(vm, v, GET_STRING(), peek(vm))) {
			UNWIND_STACK(vm);
		}
		DISPATCH();
	}
	TARGET(OP_ARR_GET): {
		Value i = pop(vm);
		if(!IS_NUM(i)) {
			blRise(vm, "TypeError", "Index of array access must be a number.");
			UNWIND_STACK(vm);
		}

		double index = AS_NUM(i);
		if((int64_t) index != index) {
			blRise(vm, "TypeError", "Index of array access must be an integer.");
			UNWIND_STACK(vm);
		}

		Value o = peek(vm);
		if(IS_LIST(o)) {
			ObjList *lst = AS_LIST(o);
			if(index < 0 || index >= lst->count) {
				blRise(vm, "IndexOutOfBoundException",
				    "List index out of bound: %d.", (int) index);
				UNWIND_STACK(vm);
			}

			pop(vm);
			push(vm, lst->arr[(size_t)index]);
		} else if(IS_STRING(o)) {
			ObjString *s = AS_STRING(o);
			if(index < 0 || index >= s->length) {
				blRise(vm, "IndexOutOfBoundException",
				    "String index out of bound: %d.", (int) index);
				UNWIND_STACK(vm);
			}

			char c = s->data[(size_t)index];
			ObjString *strc = copyString(vm, &c, 1);

			pop(vm);
			push(vm, OBJ_VAL(strc));
		} else {
			ObjClass *cls = getClass(vm, o);
			blRise(vm, "TypeException", "Operand of get `[]` must be "
			        "a String or a List, instead got %s.", cls->name->data);
			UNWIND_STACK(vm);
		}
		DISPATCH();
	}
	TARGET(OP_ARR_SET): {
		Value i = pop(vm);
		if(!IS_NUM(i)) {
			blRise(vm, "TypeError", "Index of array access must be a number.");
			UNWIND_STACK(vm);
		}

		double index = AS_NUM(i);
		if((int64_t) index != index) {
			blRise(vm, "TypeError", "Index of array access must be an integer.");
			UNWIND_STACK(vm);
		}

		Value o = pop(vm);
		if(IS_LIST(o)) {
			ObjList *lst = AS_LIST(o);
			if(index < 0 || index >= lst->count) {
				blRise(vm, "IndexOutOfBoundException",
					"List index out of bound: %d.", (int) index);
				UNWIND_STACK(vm);
			}

			lst->arr[(size_t)index] = peek(vm);
		} else {
			blRise(vm, "TypeException", "Operand of set `[]` must be a List.");
			return false;
		}
		DISPATCH();
	}
	TARGET(OP_JUMP): {
		int16_t off = NEXT_SHORT();
		frame->ip += off;
		DISPATCH();
	}
	TARGET(OP_JUMPF): {
		int16_t off = NEXT_SHORT();
		if(!isValTrue(pop(vm))) frame->ip += off;
		DISPATCH();
	}
	TARGET(OP_JUMPT): {
		int16_t off = NEXT_SHORT();
		if(isValTrue(pop(vm))) frame->ip += off;
		DISPATCH();
	}
	TARGET(OP_NULL):
		push(vm, NULL_VAL);
		DISPATCH();
	TARGET(OP_CALL): {
		uint8_t argc = NEXT_CODE();
		goto call;
	TARGET(OP_CALL_0):
	TARGET(OP_CALL_1):
	TARGET(OP_CALL_2):
	TARGET(OP_CALL_3):
	TARGET(OP_CALL_4):
	TARGET(OP_CALL_5):
	TARGET(OP_CALL_6):
	TARGET(OP_CALL_7):
	TARGET(OP_CALL_8):
	TARGET(OP_CALL_9):
	TARGET(OP_CALL_10):
		argc = op - OP_CALL_0;
call:
		if(!callValue(vm, peekn(vm, argc), argc)) {
			UNWIND_STACK(vm);
		}
		frame = &vm->frames[vm->frameCount - 1];
		DISPATCH();
	}
	TARGET(OP_INVOKE): {
		uint8_t argc = NEXT_CODE();
		goto invoke;
	TARGET(OP_INVOKE_0):
	TARGET(OP_INVOKE_1):
	TARGET(OP_INVOKE_2):
	TARGET(OP_INVOKE_3):
	TARGET(OP_INVOKE_4):
	TARGET(OP_INVOKE_5):
	TARGET(OP_INVOKE_6):
	TARGET(OP_INVOKE_7):
	TARGET(OP_INVOKE_8):
	TARGET(OP_INVOKE_9):
	TARGET(OP_INVOKE_10):
		argc = op - OP_INVOKE_0;
invoke:
		if(!invokeFromValue(vm, GET_STRING(), argc)) {
			UNWIND_STACK(vm);
		}
		frame = &vm->frames[vm->frameCount - 1];
		DISPATCH();
	}
	TARGET(OP_SUPER): {
		uint8_t argc = NEXT_CODE();
		goto sup_invoke;
	TARGET(OP_SUPER_0):
	TARGET(OP_SUPER_1):
	TARGET(OP_SUPER_2):
	TARGET(OP_SUPER_3):
	TARGET(OP_SUPER_4):
	TARGET(OP_SUPER_5):
	TARGET(OP_SUPER_6):
	TARGET(OP_SUPER_7):
	TARGET(OP_SUPER_8):
	TARGET(OP_SUPER_9):
	TARGET(OP_SUPER_10):
		argc = op - OP_SUPER_0;
sup_invoke:;
		ObjInstance *inst = AS_INSTANCE(peekn(vm, argc));
		if(!invokeMethod(vm, inst->base.cls->superCls, GET_STRING(), argc)) {
			UNWIND_STACK(vm);
		}
		frame = &vm->frames[vm->frameCount - 1];
		DISPATCH();
	}
	TARGET(OP_RETURN): {
		Value ret = pop(vm);

		vm->frameCount--;
		if(vm->frameCount == 0) {
			return true;
		}

		vm->sp = frame->stack;
		push(vm, ret);

		frame = &vm->frames[vm->frameCount - 1];
		vm->module = frame->fn->module;
		DISPATCH();
	}
	TARGET(OP_IMPORT_AS): {
	TARGET(OP_IMPORT):;
		ObjString *name = GET_STRING();
		if(!importModule(vm, name)) {
			blRise(vm, "ImportException", "Cannot load module `%s`.", name->data);
			UNWIND_STACK(vm);
		}

		//define name for the module in the importing module
		hashTablePut(&vm->module->globals, op == OP_IMPORT ?
					name : GET_STRING(), OBJ_VAL(getModule(vm, name)));

		//call the module's main if first time import
		if(!valueEquals(peek(vm), NULL_VAL)) {
			callValue(vm, peek(vm), 0);
			frame = &vm->frames[vm->frameCount - 1];
		}
		DISPATCH();
	}
	TARGET(OP_APPEND_LIST): {
		ObjList *l = AS_LIST(peek2(vm));

		listAppend(vm, l, peek(vm));
		pop(vm);
		DISPATCH();
	}
	TARGET(OP_NEW_LIST):
		push(vm, OBJ_VAL(newList(vm, 0)));
		DISPATCH();
	TARGET(OP_NEW_CLASS):
		createClass(vm, GET_STRING(), vm->objClass);
		DISPATCH();
	TARGET(OP_NEW_SUBCLASS):
		if(!IS_CLASS(peek(vm))) {
			blRise(vm, "TypeException", "Superclass in class declaration must be a Class.");
			UNWIND_STACK(vm);
		}
		createClass(vm, GET_STRING(), AS_CLASS(pop(vm)));
		DISPATCH();
	TARGET(OP_DEF_METHOD): {
		ObjClass *cls = AS_CLASS(peek(vm));
		ObjString *methodName = GET_STRING();
		hashTablePut(&cls->methods, methodName, GET_CONST());
		DISPATCH();
	}
	TARGET(OP_NAT_METHOD): {
		ObjClass *cls = AS_CLASS(peek(vm));
		ObjString *methodName = GET_STRING();
		ObjNative *native = AS_NATIVE(GET_CONST());

		native->fn = resolveBuiltIn(vm->module->name->data, cls->name->data, methodName->data);
		if(native->fn == NULL) {
			blRise(vm, "Exception", "Cannot resolve native method %s().", native->name->data);
			UNWIND_STACK(vm);
		}

		hashTablePut(&cls->methods, methodName, OBJ_VAL(native));
		DISPATCH();
	}
	TARGET(OP_DEFINE_NATIVE): {
		ObjString *name = GET_STRING();
		ObjNative *nat  = AS_NATIVE(pop(vm));

		nat->fn = resolveBuiltIn(vm->module->name->data, NULL, name->data);
		if(nat->fn == NULL) {
			blRise(vm, "Exception", "Cannot resolve native %s.", nat->name->data);
			UNWIND_STACK(vm);
		}

		hashTablePut(&vm->module->globals, name, OBJ_VAL(nat));
		DISPATCH();
	}
	TARGET(OP_GET_CONST):
		push(vm, GET_CONST());
		DISPATCH();
	TARGET(OP_DEFINE_GLOBAL):
		hashTablePut(&vm->module->globals, GET_STRING(), pop(vm));
		DISPATCH();
	TARGET(OP_GET_GLOBAL): {
		ObjString *name = GET_STRING();
		if(!hashTableGet(&vm->module->globals, name, vm->sp++)) {
			blRise(vm, "NameException", "Name `%s` is not defined.", name->data);
			UNWIND_STACK(vm);
		}
		DISPATCH();
	}
	TARGET(OP_SET_GLOBAL): {
		ObjString *name = GET_STRING();
		if(hashTablePut(&vm->module->globals, name, peek(vm))) {
			blRise(vm, "NameException", "Name `%s` is not defined.", name->data);
			UNWIND_STACK(vm);
		}
		DISPATCH();
	}
	TARGET(OP_SETUP_TRY): {
		uint16_t handlerOff = NEXT_SHORT();
		Handler *handler = &frame->handlers[frame->handlerc++];
		handler->handler = frame->ip + handlerOff;
		handler->savesp = vm->sp;
		DISPATCH();
	}
	TARGET(OP_EXC_HANDLED): {
		vm->exception = NULL;
		frame->handlerc--;
		sbuf_clear(&vm->stacktrace);
		DISPATCH();
	}
	TARGET(OP_END_TRY):
		if(vm->exception != NULL) {
			frame->handlerc--;
			UNWIND_STACK(vm);
		}
		DISPATCH();
	TARGET(OP_RAISE): {
		sbuf_clear(&vm->stacktrace);
		Value exc = pop(vm);

		if(!IS_INSTANCE(exc)) {
			blRise(vm, "TypeException", "Can only raise object instances.");
			UNWIND_STACK(vm);
		}

		vm->exception = AS_OBJ(exc);

		UNWIND_STACK(vm);
	}
	TARGET(OP_GET_LOCAL):
		push(vm, frame->stack[NEXT_CODE()]);
		DISPATCH();
	TARGET(OP_SET_LOCAL):
		frame->stack[NEXT_CODE()] = peek(vm);
		DISPATCH();
	TARGET(OP_POP):
		pop(vm);
		DISPATCH();
	TARGET(OP_DUP):
		*vm->sp = *(vm->sp - 1);
		vm->sp++;
		DISPATCH();
	}

	}

	#undef NEXT_CODE
	#undef NEXT_SHORT
	#undef GET_CONST
	#undef GET_STRING
	#undef BINARY
	#undef PRINT_DBG_STACK
	#undef CASE
	#undef TARGET
	#undef DISPATCH
}

EvalResult evaluate(VM *vm, const char *fpath, const char *src) {
	return evaluateModule(vm, fpath, "__main__", src);
}

EvalResult evaluateModule(VM *vm, const char *fpath, const char *module, const char *src) {
	Parser p;

	Stmt *program = parse(&p, fpath, src);
	if(p.hadError) {
		freeStmt(program);
		return VM_SYNTAX_ERR;
	}

	ObjString *name = copyString(vm, module, strlen(module));
	ObjFunction *fn = compileWithModule(vm, name, program);

	freeStmt(program);
	if(fn == NULL) {
		return VM_COMPILE_ERR;
	}

	callFunction(vm, fn, 0);

	if(!runEval(vm)) {
		return VM_RUNTIME_ERR;
	}

	return VM_EVAL_SUCCSESS;
}

static void printStackTrace(VM *vm) {
	fprintf(stderr, "Traceback (most recent call last):\n");

	// Print stacktrace in reverse order of recording (most recent call last)
	char *st = sbuf_get_backing_buf(&vm->stacktrace);
	int lastnl = sbuf_get_len(&vm->stacktrace);
	for(int i = lastnl; i > 0; i--) {
		if(st[i - 1] == '\n') {
			fprintf(stderr, "%.*s", lastnl - i, st + i);
			lastnl = i;
		}
	}
	fprintf(stderr, "%.*s", lastnl, st);

	// print the exception instance information
	Value v;
	ObjInstance *exc = (ObjInstance*)vm->exception;
	bool found = hashTableGet(&exc->fields, copyString(vm, "err", 3), &v);

	if(found && IS_STRING(v)) {
		fprintf(stderr, "%s: %s\n", exc->base.cls->name->data, AS_STRING(v)->data);
	} else {
		fprintf(stderr, "%s\n", exc->base.cls->name->data);
	}
}

static bool unwindStack(VM *vm) {
	for(;vm->frameCount > 0; vm->frameCount--) {
		Frame *f = &vm->frames[vm->frameCount - 1];

		// if current frame has except handlers
		if(f->handlerc > 0) {
			Handler *h = &f->handlers[f->handlerc - 1];

			// restore vm state and set ip to handler start
			f->ip = h->handler;
			vm->sp = h->savesp;
			vm->module = f->fn->module;

			// push the exception for use in the handler and return to execution
			push(vm, OBJ_VAL(vm->exception));
			return true;
		}

		// if no handler is encountered save stack info to stacktrace
		ObjFunction *fn = f->fn;
		size_t op = f->ip - fn->chunk.code - 1;

		char line[MAX_STRLEN_FOR_INT_TYPE(int) + 1] = { 0 };
		sprintf(line, "%d", getBytecodeSrcLine(&fn->chunk, op));
		sbuf_appendstr(&vm->stacktrace, "    [line ");
		sbuf_appendstr(&vm->stacktrace, line);
		sbuf_appendstr(&vm->stacktrace, "] ");

		sbuf_appendstr(&vm->stacktrace, "module ");
		sbuf_appendstr(&vm->stacktrace, fn->module->name->data);
		sbuf_appendstr(&vm->stacktrace, " in ");

		if(fn->name != NULL) {
			sbuf_appendstr(&vm->stacktrace, fn->name->data);
			sbuf_appendstr(&vm->stacktrace, "()\n");
		} else {
			sbuf_appendstr(&vm->stacktrace, "<main>\n");
		}
	}

	// We have reached the bottom of the stack, print the stacktrace and exit
	printStackTrace(vm);
	reset(vm);
	return false;
}

void blInitCommandLineArgs(int argc, const char **argv) {
	sysInitArgs(argc, argv);
}

void blAddImportPath(VM *vm, const char *path) {
	listAppend(vm, vm->importpaths, OBJ_VAL(copyString(vm, path, strlen(path))));
}

void freeVM(VM *vm) {
	reset(vm);

	sbuf_destroy(&vm->stacktrace);

	freeHashTable(&vm->strings);
	freeHashTable(&vm->modules);
	freeObjects(vm);

	free(vm->stack);
	free(vm->frames);

#ifdef DBG_PRINT_GC
	printf("Allocated at exit: %lu bytes.\n", vm->allocated);
#endif
}
