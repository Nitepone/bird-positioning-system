// On-device models for validation (validate.h) on ESP32 boards: TFLite Micro
// as the Arduino core bundles it (esp-tflite-micro with esp-nn's optimised
// kernels on the ESP32-S3), a worker task, and PSRAM buffers.
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <new>
#include <stdio.h>
#include <string.h>

extern "C" {
#include "port.h"
}

#if CONFIG_IDF_TARGET_ESP32S3 && __has_include("tensorflow/lite/micro/micro_interpreter.h")
#define BPS_HAS_ML 1
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"
#endif

extern "C" void *port_big_alloc(size_t n)
{
    void *p = heap_caps_aligned_alloc(16, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_aligned_alloc(16, n, MALLOC_CAP_8BIT);
}

extern "C" void port_big_free(void *p)
{
    if (p)
        heap_caps_free(p);
}

#if BPS_HAS_ML

// The ops the models in trigger-models/ use (README.md lists them).
using Resolver = tflite::MicroMutableOpResolver<16>;
alignas(Resolver) static uint8_t resolver_mem[sizeof(Resolver)];
alignas(tflite::MicroInterpreter) static uint8_t interp_mem[sizeof(tflite::MicroInterpreter)];
static Resolver *resolver;
static tflite::MicroInterpreter *interp;
static uint8_t *arena;

static Resolver *ops(void)
{
    if (!resolver) {
        resolver = new (resolver_mem) Resolver();
        resolver->AddConv2D();
        resolver->AddDepthwiseConv2D();
        resolver->AddFullyConnected();
        resolver->AddMaxPool2D();
        resolver->AddAveragePool2D();
        resolver->AddMean();
        resolver->AddSoftmax();
        resolver->AddLogistic();
        resolver->AddMul();
        resolver->AddAdd();
        resolver->AddQuantize();
        resolver->AddDequantize();
        resolver->AddTranspose();
        resolver->AddStridedSlice();
        resolver->AddReshape();
        resolver->AddRelu();
    }
    return resolver;
}

static void tensor_of(const TfLiteTensor *t, port_ml_tensor *out)
{
    out->is_int8 = t->type == kTfLiteInt8;
    out->data = t->data.raw;
    out->bytes = t->bytes;
    out->scale = t->params.scale;
    out->zero_point = t->params.zero_point;
}

extern "C" void port_ml_unload(void)
{
    if (interp)
        interp->~MicroInterpreter();
    interp = nullptr;
    port_big_free(arena);
    arena = nullptr;
}

extern "C" bool port_ml_load(const void *model, size_t model_bytes, size_t arena_bytes,
                             port_ml_tensor *in, port_ml_tensor *out, size_t *arena_used, char *err,
                             size_t cap)
{
    port_ml_unload();
    // TFLite Micro trusts the flatbuffer's offsets: a truncated or damaged
    // model would have it read past the buffer. Check every one first.
    flatbuffers::Verifier verifier((const uint8_t *)model, model_bytes);
    if (!tflite::VerifyModelBuffer(verifier)) {
        snprintf(err, cap, "model.tflite is not a well-formed model of %u bytes",
                 (unsigned)model_bytes);
        return false;
    }
    const tflite::Model *m = tflite::GetModel(model);
    if (m->version() != TFLITE_SCHEMA_VERSION) {
        snprintf(err, cap, "TFLite schema %lu, not %d", (unsigned long)m->version(), TFLITE_SCHEMA_VERSION);
        return false;
    }
    // Internal RAM is faster, but the network stack needs what there is.
    arena = (uint8_t *)port_big_alloc(arena_bytes);
    if (!arena) {
        snprintf(err, cap, "no memory for a %u KB arena", (unsigned)(arena_bytes / 1024));
        return false;
    }
    interp = new (interp_mem) tflite::MicroInterpreter(m, *ops(), arena, arena_bytes);
    if (interp->AllocateTensors() != kTfLiteOk) {
        snprintf(err, cap, "its tensors do not fit a %u KB arena, or it uses an op not built in",
                 (unsigned)(arena_bytes / 1024));
        port_ml_unload();
        return false;
    }
    if (interp->inputs_size() != 1 || interp->outputs_size() != 1) {
        snprintf(err, cap, "it has %u inputs and %u outputs, not one each",
                 (unsigned)interp->inputs_size(), (unsigned)interp->outputs_size());
        port_ml_unload();
        return false;
    }
    const TfLiteTensor *ti = interp->input(0), *to = interp->output(0);
    if ((ti->type != kTfLiteInt8 && ti->type != kTfLiteFloat32) ||
        (to->type != kTfLiteInt8 && to->type != kTfLiteFloat32)) {
        snprintf(err, cap, "its input or output is neither int8 nor float32");
        port_ml_unload();
        return false;
    }
    tensor_of(ti, in);
    tensor_of(to, out);
    *arena_used = interp->arena_used_bytes();
    return true;
}

extern "C" bool port_ml_invoke(void) { return interp && interp->Invoke() == kTfLiteOk; }

#else

extern "C" bool port_ml_load(const void *, size_t, size_t, port_ml_tensor *, port_ml_tensor *,
                             size_t *, char *err, size_t cap)
{
    snprintf(err, cap, "this board cannot run models");
    return false;
}
extern "C" bool port_ml_invoke(void) { return false; }
extern "C" void port_ml_unload(void) {}

#endif

// ---- The worker ----
//
// On core 1 at the priority of Arduino's loop(), which never blocks: the two
// share the core in turns. Not core 0, where capture runs (and a long model
// would keep the idle task, and so its watchdog, waiting).

static TaskHandle_t worker;
static void (*worker_fn)(void);

static void worker_task(void *)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        worker_fn();
    }
}

extern "C" bool port_worker_start(void (*fn)(void))
{
#if BPS_HAS_ML
    if (worker)
        return true;
    worker_fn = fn;
    return xTaskCreatePinnedToCore(worker_task, "validate", 16384, nullptr, 1, &worker, 1) == pdPASS;
#else
    (void)fn;
    return false;
#endif
}

extern "C" void port_worker_kick(void)
{
    if (worker)
        xTaskNotifyGive(worker);
}
