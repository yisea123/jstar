#ifndef FILE_H
#define FILE_H

#include "blang.h"

// class File {
#define FIELD_FILE_HANDLE "_handle"
#define FIELD_FILE_CLOSED "_closed"

NATIVE(bl_File_read);
NATIVE(bl_File_readAll);
NATIVE(bl_File_readLine);
NATIVE(bl_File_write);
NATIVE(bl_File_close);
NATIVE(bl_File_seek);
NATIVE(bl_File_tell);
NATIVE(bl_File_rewind);
NATIVE(bl_File_flush);
// } class File

// class __PFile {
NATIVE(bl_PFile_close);
// }

// prototypes

NATIVE(bl_popen);
NATIVE(bl_remove);
NATIVE(bl_rename);
NATIVE(bl_open);

#endif
