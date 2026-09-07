#pragma once

// ---------------------------------------------------------------------------
//  Совместимость заголовка ONNX Runtime с компилятором MinGW.
//
//  Заголовок `onnxruntime_c_api.h` взят у Microsoft без единой правки, чтобы
//  его можно было заменить при обновлении библиотеки простой перезаписью
//  файла. Но он размечен аннотациями анализатора кода Microsoft (_In_, _Out_
//  и подобные): компилятору MSVC они говорят, как проверять вызовы, а MinGW о
//  них ничего не знает и отказывается разбирать файл.
//
//  Аннотации не влияют на то, как функции вызываются, — это подсказки
//  проверяющему средству. Поэтому здесь они объявлены пустыми: смысл
//  заголовка не меняется, а MinGW его принимает.
//
//  Подключать надо ЭТОТ файл, а не onnxruntime_c_api.h напрямую.
// ---------------------------------------------------------------------------

#if defined(__MINGW32__) || defined(__MINGW64__) || (defined(__GNUC__) && defined(_WIN32))

#ifndef _In_
#define _In_
#endif
#ifndef _In_opt_
#define _In_opt_
#endif
#ifndef _In_z_
#define _In_z_
#endif
#ifndef _In_opt_z_
#define _In_opt_z_
#endif
#ifndef _In_reads_
#define _In_reads_(size)
#endif
#ifndef _Inout_
#define _Inout_
#endif
#ifndef _Inout_opt_
#define _Inout_opt_
#endif
#ifndef _Inout_updates_
#define _Inout_updates_(size)
#endif
#ifndef _Inout_updates_all_
#define _Inout_updates_all_(size)
#endif
#ifndef _Out_
#define _Out_
#endif
#ifndef _Out_opt_
#define _Out_opt_
#endif
#ifndef _Out_writes_
#define _Out_writes_(size)
#endif
#ifndef _Out_writes_all_
#define _Out_writes_all_(size)
#endif
#ifndef _Out_writes_bytes_all_
#define _Out_writes_bytes_all_(size)
#endif
#ifndef _Outptr_
#define _Outptr_
#endif
#ifndef _Outptr_result_maybenull_
#define _Outptr_result_maybenull_
#endif
#ifndef _Outptr_result_buffer_maybenull_
#define _Outptr_result_buffer_maybenull_(size)
#endif
#ifndef _Ret_maybenull_
#define _Ret_maybenull_
#endif
#ifndef _Ret_notnull_
#define _Ret_notnull_
#endif
#ifndef _Check_return_
#define _Check_return_
#endif
#ifndef _Frees_ptr_opt_
#define _Frees_ptr_opt_
#endif
#ifndef _Success_
#define _Success_(expr)
#endif
#ifndef _Return_type_success_
#define _Return_type_success_(expr)
#endif

#endif // MinGW

#include "onnxruntime_c_api.h"
