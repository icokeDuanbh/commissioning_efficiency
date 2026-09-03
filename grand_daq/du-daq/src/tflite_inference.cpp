#include <stdio.h>
#include <stdlib.h>
#include "/home/grand/externals/tensorflow-master/tensorflow/lite/c/c_api.h"
#include "tflite_inference.h"

// 2**13 = 8192
float G_quantif = 8192.0;

#ifdef REAL_DU

/**
 * \fn TfLiteInterpreter TFLT_create*(void)
 * \brief
 *
 * \return
 */

S_TFLite* TFLT_create (int nb_thread)
{
   const uint16_t nb_sample = TFLT_SAMPLE_IN_TRACE;
   S_TFLite *self = NULL;
   self = (S_TFLite*) malloc (sizeof(S_TFLite));

   /*
    * Tensorflow Lite init
    *
    * */
   TfLiteModel *pmodel = TfLiteModelCreateFromFile ("/home/root/grand-daq/arm/cnn_trained/trigger_grand.tflite");
   self->p_model = pmodel;
   TfLiteInterpreterOptions *poptions = TfLiteInterpreterOptionsCreate ();
   self->p_options = poptions;
   TfLiteInterpreterOptionsSetNumThreads (poptions, nb_thread);
   /* Create the interpreter.*/
   TfLiteInterpreter *interpreter = TfLiteInterpreterCreate (pmodel, poptions);
   /*Allocate tensors and populate the input tensor data.*/
   TfLiteInterpreterAllocateTensors (interpreter);
   self->p_interp = interpreter;

   /*
    * Array trace allocation
    *
    * */
   self->nb_sample = nb_sample;
   self->size_byte = sizeof(float) * nb_sample * 3;
   self->a_3dtraces = (float*) malloc (self->size_byte);

   return self;
}

/**
 * \fn int TFINF_delete(TfLiteInterpreter**)
 * \brief
 *
 * \param pp_interp
 * \return
 */

void TFLT_delete (S_TFLite **pself)
{
	S_TFLite *self = *pself;

   if (self == NULL)
      return;
   /* Dispose of the model and interpreter objects.*/
   TfLiteInterpreterDelete (self->p_interp);
   TfLiteInterpreterOptionsDelete (self->p_options);
   TfLiteModelDelete (self->p_model);
   free (self->a_3dtraces);
   free (self);
   *pself = NULL;
}

/**
 * \fn void TFLT_preprocessing(S_TFLite* const, const uint32_t* const)
 * \brief preprocessing specific to data format of dudaq version 2
 *
 * \param self
 * \param a_tr_adu
 */
void TFLT_preprocessing (S_TFLite *const self, const uint32_t *const a_tr_adu)
{

   int l_s, idx = 0;
   short val1, val2;

   for (l_s = 0; l_s < self->nb_sample / 2; l_s++)
   {
      /* X = Channel 2 */
      val2 = a_tr_adu[l_s + 512] >> 16;
      val1 = a_tr_adu[l_s + 512] & 0xffff;
      self->a_3dtraces[idx] = val1 / G_quantif;
      self->a_3dtraces[idx + 3] = val2 / G_quantif;
      /* Y = Channel 3 */
      val2 = a_tr_adu[l_s + 1024] >> 16;
      val1 = a_tr_adu[l_s + 1024] & 0xffff;
      self->a_3dtraces[idx + 1] = val1 / G_quantif;
      self->a_3dtraces[idx + 4] = val2 / G_quantif;
      /* Z = Channel 4 */
      val2 = a_tr_adu[l_s + 1536] >> 16;
      val1 = a_tr_adu[l_s + 1536] & 0xffff;
      self->a_3dtraces[idx + 2] = val1 / G_quantif;
      self->a_3dtraces[idx + 5] = val2 / G_quantif;
      idx += 6;
   }
}

/**
 * \fn void run_inference(TfLiteInterpreter*, float*, float*)
 * \brief
 *
 * \param interpreter
 * \param input
 * \param output_proba
 */

void TFLT_inference (S_TFLite *const self, float *const p_proba)
{
   TfLiteTensor *input_tensor = TfLiteInterpreterGetInputTensor (self->p_interp, 0);
   TfLiteTensorCopyFromBuffer (input_tensor, self->a_3dtraces, self->size_byte);

   /* Execute inference */
   TfLiteInterpreterInvoke (self->p_interp);

   /* Extract the output tensor data */
   const TfLiteTensor *output_tensor = TfLiteInterpreterGetOutputTensor (self->p_interp, 0);
   TfLiteTensorCopyToBuffer (output_tensor, p_proba, sizeof(float));
}

#endif