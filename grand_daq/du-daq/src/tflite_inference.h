// #include <cstdint>

// // /*
// //  * tflite_inference.h
// //  *
// //  *  Created on: 5 déc. 2023
// //  *      Author: jcolley
// //  *      modifided by duanbh on 20240606
// //  */

// // #ifndef DUDAQ_1_MYFILES_TFLITE_INFERENCE_H_
// // #define DUDAQ_1_MYFILES_TFLITE_INFERENCE_H_

// // // #include "/home/grand/externals/tensorflow/tensorflow/lite/c/c_api.h"
// // #include "tensorflow/lite/c/c_api.h"


// // #define TFLT_SAMPLE_IN_TRACE 1024

// // typedef struct
// // {
// //    TfLiteInterpreter *p_interp; // tensorflow lite structure for inference
// //    TfLiteInterpreterOptions *p_options; // tensorflow lite structure for inference
// //    TfLiteModel *p_model; // tensorflow lite structure for inference
// //    float *a_3dtraces; // array of 3d traces
// //    uint64_t size_byte; // size of array of 3d traces
// //    uint16_t nb_sample; // in one trace
// // } S_TFLite;

// // S_TFLite* TFLT_create (int nb_thread);

// // void TFLT_delete (S_TFLite **pself);

// // void TFLT_preprocessing (S_TFLite *const self, const uint32_t *const a_tr_adu);

// // void TFLT_inference (S_TFLite *const self, float *const p_proba);

// // #endif /* DUDAQ_1_MYFILES_TFLITE_INFERENCE_H_ */

// /*
//  * tflite_inference.h
//  *
//  *  Created on: 5 déc. 2023
//  *      Author: jcolley
//  *      modified by duanbh on 20240606
//  */

// #ifndef DUDAQ_1_MYFILES_TFLITE_INFERENCE_H_
// #define DUDAQ_1_MYFILES_TFLITE_INFERENCE_H_

// #define TFLT_SAMPLE_IN_TRACE 1024

// #ifdef REAL_DU
// // 只有在 REAL_DU=ON 时才包含 TensorFlow Lite 头文件
// #include "tensorflow/lite/c/c_api.h"

// typedef struct
// {
//    TfLiteInterpreter *p_interp; 
//    TfLiteInterpreterOptions *p_options; 
//    TfLiteModel *p_model; 
//    float *a_3dtraces; 
//    uint64_t size_byte; 
//    uint16_t nb_sample; 
// } S_TFLite;

// S_TFLite* TFLT_create (int nb_thread);
// void TFLT_delete (S_TFLite **pself);
// void TFLT_preprocessing (S_TFLite *const self, const uint32_t *const a_tr_adu);
// void TFLT_inference (S_TFLite *const self, float *const p_proba);

// #else
// // ===== 如果不是 REAL_DU，则给一个空壳定义，避免编译错误 =====
// typedef struct
// {
//    void *dummy;   // 占位
// } S_TFLite;

// inline S_TFLite* TFLT_create (int) { return nullptr; }
// inline void TFLT_delete (S_TFLite **) {}
// inline void TFLT_preprocessing (S_TFLite *, const uint32_t *) {}
// inline void TFLT_inference (S_TFLite *, float *) {}

// #endif  // REAL_DU

// #endif /* DUDAQ_1_MYFILES_TFLITE_INFERENCE_H_ */

// /*
//  * tflite_inference.h
//  *
//  *  Created on: 5 déc. 2023
//  *      Author: jcolley
//  *      modifided by duanbh on 20240606
//  */

// #include <cstdint>
// #ifndef DUDAQ_1_MYFILES_TFLITE_INFERENCE_H_
// #define DUDAQ_1_MYFILES_TFLITE_INFERENCE_H_

// #ifdef REAL_DU
// #include "tensorflow/lite/c/c_api.h"
// #endif

// #define TFLT_SAMPLE_IN_TRACE 1024

// typedef struct
// {
//    TfLiteInterpreter *p_interp; // tensorflow lite structure for inference
//    TfLiteInterpreterOptions *p_options; // tensorflow lite structure for inference
//    TfLiteModel *p_model; // tensorflow lite structure for inference
//    float *a_3dtraces; // array of 3d traces
//    uint64_t size_byte; // size of array of 3d traces
//    uint16_t nb_sample; // in one trace
// } S_TFLite;

// S_TFLite* TFLT_create (int nb_thread);

// void TFLT_delete (S_TFLite **pself);

// void TFLT_preprocessing (S_TFLite *const self, const uint32_t *const a_tr_adu);

// void TFLT_inference (S_TFLite *const self, float *const p_proba);


// #endif /* DUDAQ_1_MYFILES_TFLITE_INFERENCE_H_ */


/*
 * tflite_inference.h
 *
 *  Created on: 5 déc. 2023
 *      Author: jcolley
 *      modified by duanbh on 20240606
 *      updated by ChatGPT on 20251019
 */

#ifndef DUDAQ_1_MYFILES_TFLITE_INFERENCE_H_
#define DUDAQ_1_MYFILES_TFLITE_INFERENCE_H_

#include <cstdint>

// ==========================
// ✅ TensorFlow Lite 仅在 REAL_DU 模式下启用
// ==========================
#ifdef REAL_DU
#include "tensorflow/lite/c/c_api.h"

#define TFLT_SAMPLE_IN_TRACE 1024

typedef struct
{
    TfLiteInterpreter *p_interp;           // TFLite interpreter
    TfLiteInterpreterOptions *p_options;   // Interpreter options
    TfLiteModel *p_model;                  // Model pointer
    float *a_3dtraces;                     // Array of input traces
    uint64_t size_byte;                    // Memory size
    uint16_t nb_sample;                    // Number of samples per trace
} S_TFLite;

// ====== 实际实现函数声明 ======
S_TFLite* TFLT_create (int nb_thread);
void TFLT_delete (S_TFLite **pself);
void TFLT_preprocessing (S_TFLite *const self, const uint32_t *const a_tr_adu);
void TFLT_inference (S_TFLite *const self, float *const p_proba);

#else
// ==========================
// 🧩 PC/x86 模式下的空壳定义（无 TensorFlow Lite）
// ==========================
#define TFLT_SAMPLE_IN_TRACE 1024

typedef struct
{
    void *dummy;   // 占位符以避免空结构体
} S_TFLite;

// 提供空函数，确保上层能编译通过
inline S_TFLite* TFLT_create (int) { return nullptr; }
inline void TFLT_delete (S_TFLite **) {}
inline void TFLT_preprocessing (S_TFLite *, const uint32_t *) {}
inline void TFLT_inference (S_TFLite *, float *) {}

#endif  // REAL_DU

#endif /* DUDAQ_1_MYFILES_TFLITE_INFERENCE_H_ */

