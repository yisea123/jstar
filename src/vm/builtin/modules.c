#include "modules.h"
#include "object.h"

#include "core.bl.h"
#include "core.h"

#include "sys.bl.h"
#include "sys.h"

#include "io.bl.h"
#include "io.h"

#include "math.bl.h"
#include "math.h"

#include "map.bl.h"
#include "map.h"

#include "set.bl.h"

#include "debug.bl.h"
#include "debug.h"

#include "re.bl.h"
#include "re.h"

#include <string.h>

typedef enum { TYPE_FUNC, TYPE_CLASS } Type;

typedef struct {
    const char *name;
    Native func;
} Func;

typedef struct {
    const char *name;
    Func methods[16];
} Class;

typedef struct {
    Type type;
    union {
        Func function;
        Class class;
    };
} ModuleElem;

typedef struct {
    const char *name;
    const char **src;
    ModuleElem elems[31];
} Module;

// clang-format off

#define ELEMS_END        {TYPE_FUNC, .function = METHODS_END},
#define MODULES_END      {NULL, NULL, { ELEMS_END }}
#define METHODS_END      {NULL, NULL}

#define MODULE(name)     { #name, &name##_bl, {
#define ENDMODULE        ELEMS_END } },

#define COREMODULE       {"__core__", &core_bl, {

#define CLASS(name)      { TYPE_CLASS, .class = { #name, {
#define METHOD(name, fn) { #name, fn },
#define ENDCLASS         METHODS_END } } },

#define FUNCTION(name, fn) { TYPE_FUNC, .function = { #name, fn } },

Module builtInModules[] = {
    COREMODULE
        FUNCTION(int,        &bl_int)
        FUNCTION(num,        &bl_num)
        FUNCTION(isInt,      &bl_isInt)
        FUNCTION(char,       &bl_char)
        FUNCTION(ascii,      &bl_ascii)
        FUNCTION(type,       &bl_type)
        FUNCTION(__printstr, &bl_printstr)
        CLASS(Number)
            METHOD(__string__, &bl_Number_string)
            METHOD(__hash__,   &bl_Number_hash)
        ENDCLASS
        CLASS(Boolean)
            METHOD(__string__, &bl_Boolean_string)
        ENDCLASS
        CLASS(Null)
            METHOD(__string__, &bl_Null_string)
        ENDCLASS
        CLASS(Function)
            METHOD(__string__, &bl_Function_string)
        ENDCLASS
        CLASS(Module)
            METHOD(__string__, &bl_Module_string)
        ENDCLASS
        CLASS(List)
            METHOD(add,      &bl_List_add)
            METHOD(insert,   &bl_List_insert)
            METHOD(removeAt, &bl_List_removeAt)
            METHOD(clear,    &bl_List_clear)
            METHOD(subList,  &bl_List_subList)
            METHOD(__len__,  &bl_List_len)
            METHOD(__iter__, &bl_List_iter)
            METHOD(__next__, &bl_List_next)
        ENDCLASS
        CLASS(Tuple)
            METHOD(sub,      &bl_Tuple_sub)
            METHOD(__len__,  &bl_Tuple_len)
            METHOD(__iter__, &bl_Tuple_iter)
            METHOD(__next__, &bl_Tuple_next)
        ENDCLASS
        CLASS(String)
            METHOD(substr,     &bl_substr)
            METHOD(join,     &bl_String_join)
            METHOD(__eq__,     &bl_String_eq)
            METHOD(__len__,    &bl_String_len)
            METHOD(__hash__,   &bl_String_hash)
            METHOD(__iter__,   &bl_String_iter)
            METHOD(__next__,   &bl_String_next)
            METHOD(__string__, &bl_String_string)
        ENDCLASS
        FUNCTION(eval, &bl_eval)
    ENDMODULE
    MODULE(sys)
        FUNCTION(time,           &bl_time)
        FUNCTION(exec,           &bl_exec)
        FUNCTION(exit,           &bl_exit)
        FUNCTION(getImportPaths, &bl_getImportPaths)
        FUNCTION(platform,       &bl_platform)
        FUNCTION(clock,          &bl_clock)
        FUNCTION(gc,             &bl_gc)
        FUNCTION(gets,           &bl_gets)
        FUNCTION(init,           &bl_sys_init)
    ENDMODULE
    MODULE(io)
        CLASS(File)
            METHOD(read,     &bl_File_read)
            METHOD(readAll,  &bl_File_readAll)
            METHOD(readLine, &bl_File_readLine)
            METHOD(write,    &bl_File_write)
            METHOD(close,    &bl_File_close)
            METHOD(seek,     &bl_File_seek)
            METHOD(tell,     &bl_File_tell)
            METHOD(rewind,   &bl_File_rewind)
            METHOD(flush,    &bl_File_flush)
        ENDCLASS
        CLASS(__PFile)
            METHOD(close,    &bl_PFile_close)
        ENDCLASS
        FUNCTION(popen,  &bl_popen)
        FUNCTION(remove, &bl_remove)
        FUNCTION(rename, &bl_rename)
        FUNCTION(__open, &bl_open)
    ENDMODULE
    MODULE(math)
        FUNCTION(abs,    &bl_abs)
        FUNCTION(acos,   &bl_acos)
        FUNCTION(asin,   &bl_asin)
        FUNCTION(atan,   &bl_atan)
        FUNCTION(atan2,  &bl_atan2)
        FUNCTION(ceil,   &bl_ceil)
        FUNCTION(cos,    &bl_cos)
        FUNCTION(cosh,   &bl_cosh)
        FUNCTION(deg,    &bl_deg)
        FUNCTION(exp,    &bl_exp)
        FUNCTION(floor,  &bl_floor)
        FUNCTION(frexp,  &bl_frexp)
        FUNCTION(ldexp,  &bl_ldexp)
        FUNCTION(log,    &bl_log)
        FUNCTION(log10,  &bl_log10)
        FUNCTION(max,    &bl_max)
        FUNCTION(min,    &bl_min)
        FUNCTION(rad,    &bl_rad)
        FUNCTION(sin,    &bl_sin)
        FUNCTION(sinh,   &bl_sinh)
        FUNCTION(sqrt,   &bl_sqrt)
        FUNCTION(tan,    &bl_tan)
        FUNCTION(tanh,   &bl_tanh)
        FUNCTION(modf,   &bl_modf)
        FUNCTION(random, &bl_random)
        FUNCTION(seed,   &bl_seed)
        FUNCTION(init,   &bl_math_init)
    ENDMODULE
    MODULE(re)
        FUNCTION(match,  &bl_re_match)
        FUNCTION(find,   &bl_re_find)
        FUNCTION(gmatch, &bl_re_gmatch)
        FUNCTION(gsub,   &bl_re_gsub)
    ENDMODULE
    MODULE(map)
        CLASS(Map)
            METHOD(__getEntry, &bl_Map_getEntry)
            METHOD(__addEntry, &bl_Map_addEntry)
            METHOD(__grow,     &bl_Map_grow)
        ENDCLASS
    ENDMODULE
    MODULE(set) ENDMODULE
    MODULE(debug)
        FUNCTION(printStack, &bl_printStack)
        FUNCTION(dis,        &bl_dis)
    ENDMODULE
    MODULES_END
};

// clang-format on

static Module *getModule(const char *name) {
    for(int i = 0; builtInModules[i].name != NULL; i++) {
        if(strcmp(name, builtInModules[i].name) == 0) {
            return &builtInModules[i];
        }
    }
    return NULL;
}

static Class *getClass(Module *module, const char *name) {
    for(int i = 0;; i++) {
        ModuleElem *e = &module->elems[i];
        if(e->type == TYPE_FUNC && e->function.name == NULL) return NULL;

        if(e->type == TYPE_CLASS) {
            if(strcmp(module->elems[i].class.name, name) == 0) {
                return &module->elems[i].class;
            }
        }
    }
}

static Native getNativeMethod(Class *cls, const char *name) {
    for(int i = 0; cls->methods[i].name != NULL; i++) {
        if(strcmp(cls->methods[i].name, name) == 0) {
            return cls->methods[i].func;
        }
    }
    return NULL;
}

static Native getNativeFunc(Module *module, const char *name) {
    for(int i = 0;; i++) {
        if(module->elems[i].type == TYPE_FUNC) {
            if(module->elems[i].function.name == NULL) return NULL;

            if(strcmp(module->elems[i].function.name, name) == 0) {
                return module->elems[i].function.func;
            }
        }
    }
}

Native resolveBuiltIn(const char *module, const char *cls, const char *name) {
    Module *m = getModule(module);
    if(m == NULL) return NULL;

    if(cls == NULL) {
        return getNativeFunc(m, name);
    }

    Class *c = getClass(m, cls);
    if(c == NULL) return NULL;

    return getNativeMethod(c, name);
}

const char *readBuiltInModule(const char *name) {
    Module *m = getModule(name);
    if(m != NULL) {
        return *m->src;
    }
    return NULL;
}
