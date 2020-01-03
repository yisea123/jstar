#ifndef CORE_H
#define CORE_H

#include "jstar.h"

// J* core libray bootstrap
void initCoreLibrary(JStarVM *vm);

JSR_NATIVE(jsr_ascii);
JSR_NATIVE(jsr_char);
JSR_NATIVE(jsr_eval);
JSR_NATIVE(jsr_exec);
JSR_NATIVE(jsr_int);
JSR_NATIVE(jsr_print);
JSR_NATIVE(jsr_system);
JSR_NATIVE(jsr_type);

// class Number
    JSR_NATIVE(jsr_Number_new);
    JSR_NATIVE(jsr_Number_isInt);
    JSR_NATIVE(jsr_Number_string);
    JSR_NATIVE(jsr_Number_hash);
// end

// class Boolean
    JSR_NATIVE(jsr_Boolean_new);
    JSR_NATIVE(jsr_Boolean_string);
// end

// class Null
    JSR_NATIVE(jsr_Null_string);
// end

// class Function
    JSR_NATIVE(jsr_Function_string);
// end

// class Module
    JSR_NATIVE(jsr_Module_string);
// end

// class List
    JSR_NATIVE(jsr_List_new);
    JSR_NATIVE(jsr_List_add);
    JSR_NATIVE(jsr_List_insert);
    JSR_NATIVE(jsr_List_len);
    JSR_NATIVE(jsr_List_removeAt);
    JSR_NATIVE(jsr_List_clear);
    JSR_NATIVE(jsr_List_subList);
    JSR_NATIVE(jsr_List_iter);
    JSR_NATIVE(jsr_List_next);
// end

// class Tuple
    JSR_NATIVE(jsr_Tuple_new);
    JSR_NATIVE(jsr_Tuple_len);
    JSR_NATIVE(jsr_Tuple_iter);
    JSR_NATIVE(jsr_Tuple_next);
    JSR_NATIVE(jsr_Tuple_sub);
// end

// class String
    JSR_NATIVE(jsr_String_new);
    JSR_NATIVE(jsr_String_substr);
    JSR_NATIVE(jsr_String_startsWith);
    JSR_NATIVE(jsr_String_endsWith);
    JSR_NATIVE(jsr_String_strip);
    JSR_NATIVE(jsr_String_chomp);
    JSR_NATIVE(jsr_String_join);
    JSR_NATIVE(jsr_String_len);
    JSR_NATIVE(jsr_String_string);
    JSR_NATIVE(jsr_String_hash);
    JSR_NATIVE(jsr_String_eq);
    JSR_NATIVE(jsr_String_iter);
    JSR_NATIVE(jsr_String_next);
// end

// class Table
    JSR_NATIVE(jsr_Table_get);
    JSR_NATIVE(jsr_Table_set);
    JSR_NATIVE(jsr_Table_len);
    JSR_NATIVE(jsr_Table_delete);
    JSR_NATIVE(jsr_Table_clear);
    JSR_NATIVE(jsr_Table_contains);
    JSR_NATIVE(jsr_Table_keys);
    JSR_NATIVE(jsr_Table_values);
    JSR_NATIVE(jsr_Table_iter);
    JSR_NATIVE(jsr_Table_next);
    JSR_NATIVE(jsr_Table_string);
// end

// class Exception
    JSR_NATIVE(jsr_Exception_printStacktrace);
// end

#endif