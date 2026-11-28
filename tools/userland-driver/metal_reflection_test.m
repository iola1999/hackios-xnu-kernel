/* Copyright © 2026 Darwin on annibale contributors
 * SPDX-License-Identifier: MIT
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

#include "air_metal_reflection.h"
/* Keep the fixture subclasses distinct from the classes in a dynamically loaded driver. */
#define AnnibaleMetalComputeReflection AnnibaleMetalTestComputeReflection
#define AnnibaleMetalRenderReflection AnnibaleMetalTestRenderReflection
#include "annibalemetal_reflection.inc"
#undef AnnibaleMetalRenderReflection
#undef AnnibaleMetalComputeReflection

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "native reflection:%d: %s\n", __LINE__, #x); return 1; } } while (0)

static int
constructor_checks(void)
{
   @autoreleasepool {
      const struct rwa_reflection_type fp = { .data_type = 3, .data_size = 4,
         .alignment = 4, .name = "float" };
      const struct rwa_reflection_type fp2 = { .data_type = 4, .data_size = 8,
         .alignment = 8, .name = "float2" };
      const struct rwa_reflection_type integer = { .data_type = 29, .data_size = 4,
         .alignment = 4, .name = "int" };
      const struct rwa_reflection_type array = { .data_type = 2, .data_size = 32,
         .alignment = 8, .array_length = 4, .array_stride = 8, .element = &fp2 };
      const struct rwa_reflection_member members[] = {
         { .name = "points", .offset = 0, .type = &array },
         { .name = "count", .offset = 32, .type = &integer },
      };
      const struct rwa_reflection_type record = { .data_type = 1, .data_size = 40,
         .alignment = 8, .name = "TestParams", .member_count = 2, .members = members };
      struct rwa_reflection_argument arguments[] = {
         { .name = "params", .type = 0, .index = 0, .active = 1, .array_length = 1,
           .buffer_type = &record, .constant_buffer = 1 },
         { .name = "optionalInput", .type = 2, .index = 0, .array_length = 1,
           .texture_type = 2, .texture_data_type = 16 },
         { .name = "output", .type = 2, .index = 1, .active = 1, .array_length = 1,
           .access = 2, .texture_type = 2, .texture_data_type = 3 },
         { .name = "filter", .type = 3, .index = 2, .active = 1, .array_length = 1 },
         { .name = "shared", .type = 1, .index = 0, .active = 1, .array_length = 1,
           .access = 1, .buffer_type = &fp },
         { .name = "weight", .type = 0, .index = 1, .active = 1, .array_length = 1,
           .buffer_type = &fp, .constant_buffer = 1 },
      };
      const struct rwa_compute_reflection metadata = { .argument_count = 6,
         .arguments = arguments };
      NSString *why = nil;
      MTLComputePipelineReflection *result = rw_compute_reflection(&metadata, &why);
      if (!result) {
         fprintf(stderr, "%s\n", [why UTF8String]);
         return 1;
      }
      CHECK([result isKindOfClass:[MTLComputePipelineReflection class]]);
      CHECK([[result bindings] count] == 6 && [[result arguments] count] == 6);
      for (id<MTLBinding> binding in [result bindings]) {
         CHECK([binding isArgument]);
         MTLArgument *argument = nil;
         for (MTLArgument *candidate in [result arguments])
            if ([candidate.name isEqualToString:[binding name]])
               argument = candidate;
         CHECK(argument && (NSInteger)argument.type == (NSInteger)[binding type]);
         CHECK(argument.index == [binding index] && argument.access == [binding access]);
      }
      id<MTLBufferBinding> buffer = (id<MTLBufferBinding>)[result bindings][0];
      CHECK([buffer index] == 0 && [buffer bufferDataType] == MTLDataTypeStruct);
      CHECK([buffer bufferDataSize] == 40 && [buffer bufferAlignment] == 8);
      CHECK([buffer bufferPointerType].elementType == MTLDataTypeStruct);
      MTLStructType *type = [buffer bufferStructType];
      CHECK([[type members] count] == 2);
      MTLStructMember *member = [type memberByName:@"points"];
      CHECK(member && [member offset] == 0 && [member dataType] == MTLDataTypeArray);
      CHECK([member arrayType].arrayLength == 4 && [member arrayType].stride == 8);
      CHECK([member arrayType].elementType == MTLDataTypeFloat2);
      member = [type memberByName:@"count"];
      CHECK(member && [member offset] == 32 && [member dataType] == MTLDataTypeInt);
      id<MTLTextureBinding> input = (id<MTLTextureBinding>)[result bindings][1];
      CHECK(![input isUsed] && [input textureType] == MTLTextureType2D);
      CHECK([input textureDataType] == MTLDataTypeHalf && [input access] == 0);
      id<MTLTextureBinding> output = (id<MTLTextureBinding>)[result bindings][2];
      CHECK([output isUsed] && [output index] == 1 && [output access] == 2);
      id<MTLBinding> sampler = [result bindings][3];
      CHECK([sampler type] == MTLBindingTypeSampler && [sampler index] == 2);
      id<MTLThreadgroupBinding> shared = (id<MTLThreadgroupBinding>)[result bindings][4];
      CHECK([shared threadgroupMemoryAlignment] == 4 && [shared threadgroupMemoryDataSize] == 4);
      buffer = (id<MTLBufferBinding>)[result bindings][5];
      CHECK([buffer bufferDataType] == MTLDataTypeFloat && [buffer bufferDataSize] == 4);
      CHECK([buffer bufferStructType] == nil && [buffer bufferPointerType].elementType == MTLDataTypeFloat);
      [result release];
      arguments[0].buffer_type = &array;
      CHECK(!rw_compute_reflection(&metadata, &why) && why != nil);
      arguments[0].buffer_type = &record;
      arguments[0].name = "\xff";
      CHECK(!rw_compute_reflection(&metadata, &why) && [why containsString:@"UTF-8"]);
      arguments[0].name = "params";
      struct rwa_reflection_type invalid = record;
      invalid.alignment = UINT16_MAX + 1u;
      arguments[0].buffer_type = &invalid;
      CHECK(!rw_compute_reflection(&metadata, &why) && [why containsString:@"alignment"]);
      invalid.alignment = record.alignment;
      invalid.data_size = UINT32_MAX;
      CHECK(!rw_compute_reflection(&metadata, &why) && [why containsString:@"size"]);

      /* Render reflection: vertex with VFX-shaped argument buffer + fragment, plus depth-only. */
      const struct rwa_reflection_type f4 = { .data_type = 6, .data_size = 16,
         .alignment = 16, .name = "float4" };
      const struct rwa_reflection_type tex_m = { .data_type = 58, .data_size = 8,
         .alignment = 8, .name = "texture2d<float, sample>",
         .texture_type = 2, .texture_data_type = 3, .depth_texture = 0, .access = 0 };
      const struct rwa_reflection_type samp_m = { .data_type = 59, .data_size = 8,
         .alignment = 8, .name = "sampler" };
      const struct rwa_reflection_type ptr_m = { .data_type = 60, .data_size = 8,
         .alignment = 8, .name = "device TestParams*", .element = &record, .access = 1,
         .constant_buffer = 0 };
      const struct rwa_reflection_member nested_members[] = {
         { .name = "points", .offset = 0, .indirect_argument_index = 30, .type = &array },
      };
      const struct rwa_reflection_type nested_st = {
         .data_type = 1, .data_size = 32, .alignment = 8,
         .name = "NestedArgs", .member_count = 1, .members = nested_members,
      };
      const struct rwa_reflection_member argbuf_members[] = {
         { .name = "tint", .offset = 0, .indirect_argument_index = 0, .type = &f4 },
         { .name = "buf0", .offset = 16, .indirect_argument_index = 10, .type = &ptr_m },
         { .name = "tex0", .offset = 24, .indirect_argument_index = 26, .type = &tex_m },
         { .name = "samp0", .offset = 32, .indirect_argument_index = 27, .type = &samp_m },
         { .name = "nested", .offset = 40, .indirect_argument_index = 29, .type = &nested_st },
      };
      const struct rwa_reflection_type argbuf_st = {
         .data_type = 1, .data_size = 72, .alignment = 16,
         .name = "VFXPassArgs", .member_count = 5, .members = argbuf_members,
         .is_argument_buffer = 1,
      };
      const struct rwa_reflection_type void_t = {
         .data_type = 0, .data_size = 0, .alignment = 1, .name = "void",
      };
      const struct rwa_reflection_argument v_args[] = {
         { .name = "passArgs", .type = 0, .index = 0, .active = 1, .array_length = 1,
           .buffer_type = &argbuf_st, .constant_buffer = 1 },
         { .name = "accel", .type = 26, .index = 1, .active = 1, .array_length = 1 },
         { .name = "primAccel", .type = 25, .index = 2, .active = 1, .array_length = 1 },
         { .name = "ift", .type = 27, .index = 4, .active = 1, .array_length = 1 },
         { .name = "untyped", .type = 0, .index = 6, .active = 1, .array_length = 1,
           .buffer_type = &void_t, .constant_buffer = 0 },
      };
      const struct rwa_reflection_argument f_args[] = {
         { .name = "albedo", .type = 2, .index = 3, .active = 1, .array_length = 1,
           .texture_type = 2, .texture_data_type = 16 },
      };
      const struct rwa_compute_reflection v_refl = { .argument_count = 5, .arguments = v_args };
      const struct rwa_compute_reflection f_refl = { .argument_count = 1, .arguments = f_args };
      const struct rwa_render_reflection r_meta = { .vertex = &v_refl, .fragment = &f_refl };
      MTLRenderPipelineReflection *rr = rw_render_reflection(&r_meta, &why);
      CHECK(rr != nil && [rr isKindOfClass:[MTLRenderPipelineReflection class]]);
      CHECK([[rr vertexBindings] count] == 5 && [[rr vertexArguments] count] == 5);
      CHECK([[rr fragmentBindings] count] == 1 && [[rr fragmentArguments] count] == 1);
      id<MTLBufferBinding> vb = (id<MTLBufferBinding>)[rr vertexBindings][0];
      CHECK([vb index] == 0 && [vb bufferDataSize] == 72 && [vb bufferAlignment] == 16);
      CHECK([vb bufferPointerType].elementIsArgumentBuffer == YES);
      MTLStructType *vst = [vb bufferStructType];
      CHECK(vst != nil && [[vst members] count] == 5);
      MTLStructMember *m_tex = [vst memberByName:@"tex0"];
      CHECK(m_tex != nil && [m_tex dataType] == MTLDataTypeTexture && [m_tex argumentIndex] == 26);
      MTLTextureReferenceType *trt = [m_tex textureReferenceType];
      CHECK(trt != nil && trt.textureType == MTLTextureType2D &&
            trt.textureDataType == MTLDataTypeFloat && trt.access == MTLBindingAccessReadOnly &&
            !trt.isDepthTexture);
      MTLStructMember *m_ptr = [vst memberByName:@"buf0"];
      CHECK(m_ptr != nil && [m_ptr dataType] == MTLDataTypePointer && [m_ptr argumentIndex] == 10);
      MTLPointerType *pt = [m_ptr pointerType];
      CHECK(pt != nil && pt.elementType == MTLDataTypeStruct && pt.access == MTLBindingAccessReadWrite &&
            pt.alignment == 8 && pt.dataSize == 40 && !pt.elementIsArgumentBuffer &&
            [pt elementStructType] != nil && [[[pt elementStructType] members] count] == 2);
      MTLStructMember *m_nested = [vst memberByName:@"nested"];
      CHECK(m_nested != nil && [m_nested dataType] == MTLDataTypeStruct && [m_nested argumentIndex] == 29);
      MTLStructMember *m_pts = [[m_nested structType] memberByName:@"points"];
      CHECK(m_pts != nil && [m_pts dataType] == MTLDataTypeArray && [m_pts argumentIndex] == 30 &&
            [m_pts arrayType].arrayLength == 4);
      id<MTLBinding> b_as = [rr vertexBindings][1];
      CHECK([b_as type] == 26 && [b_as index] == 1);
      id<MTLBinding> b_pas = [rr vertexBindings][2];
      CHECK([b_pas type] == 25 && [b_pas index] == 2);
      id<MTLBinding> b_ift = [rr vertexBindings][3];
      CHECK([b_ift type] == 27 && [b_ift index] == 4);
      id<MTLBufferBinding> b_void = (id<MTLBufferBinding>)[rr vertexBindings][4];
      CHECK([b_void type] == MTLBindingTypeBuffer && [b_void bufferDataType] == MTLDataTypeNone && [b_void bufferDataSize] == 0);
      [rr release];

      /* Depth-only (fragment == NULL) and generated-vertex (vertex == NULL) return empty arrays. */
      const struct rwa_render_reflection depth_only = { .vertex = &v_refl, .fragment = NULL };
      MTLRenderPipelineReflection *dr = rw_render_reflection(&depth_only, &why);
      CHECK(dr != nil && [[dr vertexBindings] count] == 5 && [[dr fragmentBindings] count] == 0);
      [dr release];

      puts("PASS native Metal reflection: compute + render (argbuf indirectArgumentIndex, depth-only)");
   }
   return 0;
}

static id
binding_named(NSArray *bindings, NSString *name)
{
   for (id binding in bindings)
      if ([[binding name] isEqualToString:name])
         return binding;
   return nil;
}

static int
texture_declaration(NSArray *bindings, NSString *name, NSUInteger index,
                    MTLArgumentAccess access, MTLDataType dataType)
{
   id<MTLTextureBinding> texture = binding_named(bindings, name);
   CHECK(texture && [texture type] == MTLBindingTypeTexture);
   CHECK([texture index] == index && [texture access] == access && [texture isUsed]);
   CHECK([texture textureType] == MTLTextureType2D && [texture textureDataType] == dataType);
   CHECK(![texture isDepthTexture]);
   return 0;
}

static int
buffer_declaration(NSArray *bindings, NSString *name, NSUInteger index,
                   MTLDataType dataType, NSUInteger size, NSUInteger alignment)
{
   id<MTLBufferBinding> buffer = binding_named(bindings, name);
   CHECK(buffer && [buffer type] == MTLBindingTypeBuffer);
   CHECK([buffer index] == index && [buffer access] == MTLArgumentAccessReadOnly);
   CHECK([buffer isUsed] && [buffer bufferDataType] == dataType);
   CHECK([buffer bufferDataSize] == size && [buffer bufferAlignment] == alignment);
   CHECK([buffer bufferPointerType].elementType == dataType);
   return 0;
}

static int
slm_declarations(MTLComputePipelineReflection *reflection, NSString *entry)
{
   BOOL minimum = [entry isEqualToString:@"slm_shiftmap_calcminmax"];
   BOOL eyes = [entry isEqualToString:@"slm_calc_eyes"];
   CHECK(minimum || eyes || [entry isEqualToString:@"slm_calc"]);
   NSArray *bindings = [reflection bindings], *arguments = [reflection arguments];
   NSUInteger count = minimum ? 2 : eyes ? 6 : 4;
   CHECK(bindings && arguments && bindings.count == count && arguments.count == count);
   for (id<MTLBinding> binding in bindings) {
      CHECK([binding isArgument] && [binding name].length);
      MTLArgument *argument = binding_named(arguments, [binding name]);
      CHECK(argument && (NSInteger)argument.type == (NSInteger)[binding type]);
      CHECK(argument.index == [binding index] && argument.access == [binding access]);
   }
   if (minimum) {
      CHECK(!texture_declaration(bindings, @"inputBlurMapTex", 0,
                                 MTLArgumentAccessReadOnly, MTLDataTypeHalf));
      CHECK(!texture_declaration(bindings, @"outputMinMaxParamsTex", 1,
                                 MTLArgumentAccessWriteOnly, MTLDataTypeFloat));
      return 0;
   }
   CHECK(!texture_declaration(bindings, @"minMaxParamsTex", 0,
                              MTLArgumentAccessReadOnly, MTLDataTypeFloat));
   CHECK(!texture_declaration(bindings, @"inputShiftMapTex", 1,
                              MTLArgumentAccessReadOnly, MTLDataTypeHalf));
   CHECK(!texture_declaration(bindings, @"outputParamTexture", 2,
                              MTLArgumentAccessWriteOnly, MTLDataTypeFloat));
   CHECK(!buffer_declaration(bindings, @"inputParams", 0, MTLDataTypeStruct, 56, 4));
   id<MTLBufferBinding> parameters = binding_named(bindings, @"inputParams");
   MTLStructType *type = [parameters bufferStructType];
   const char *names[] = { "left", "top", "width", "height", "zeroShiftPercentile",
      "simulatedFocalLength", "simulatedAperture", "minimumSimulatedAperture",
      "maximumSimulatedAperture", "frameNormalizedFocalLength", "maxFGBlur",
      "maxBGBlur", "shiftDeadZone", "disparityScalingFactor" };
   CHECK(type.members.count == 14);
   for (NSUInteger i = 0; i < 14; i++) {
      MTLStructMember *member = [type memberByName:[NSString stringWithUTF8String:names[i]]];
      CHECK(member && member.offset == i * 4 && member.dataType == MTLDataTypeFloat);
   }
   if (!eyes)
      return 0;
   CHECK(!buffer_declaration(bindings, @"inputEyes", 1, MTLDataTypeStruct, 72, 8));
   parameters = binding_named(bindings, @"inputEyes");
   type = [parameters bufferStructType];
   CHECK(type.members.count == 3);
   for (NSUInteger i = 0; i < 2; i++) {
      MTLStructMember *member = [type memberByName:i ? @"rightEyes" : @"leftEyes"];
      CHECK(member && member.offset == i * 32 && member.dataType == MTLDataTypeArray);
      CHECK(member.arrayType.arrayLength == 4 && member.arrayType.stride == 8);
      CHECK(member.arrayType.elementType == MTLDataTypeFloat2);
   }
   MTLStructMember *member = [type memberByName:@"numValidEyes"];
   CHECK(member && member.offset == 64 && member.dataType == MTLDataTypeInt);
   CHECK(!buffer_declaration(bindings, @"nearestEyeWeight", 2, MTLDataTypeFloat, 4, 4));
   return 0;
}

static int
pipeline_check(const char *path, const char *function_name)
{
   unsetenv("RWA_SOFTWARE");
   if (!dlopen("/annibale/lib/libannibalemetal.dylib", RTLD_NOW | RTLD_LOCAL)) {
      fprintf(stderr, "reflection pipeline: driver load failed: %s\n", dlerror());
      return 2;
   }
   Class cls = objc_getClass("AnnibaleMetalDevice");
   SEL initialize = sel_registerName("initWithAcceleratorPort:");
   CHECK(cls && [cls instancesRespondToSelector:initialize]);
   id<MTLDevice> device = ((id (*)(id, SEL, uint32_t))objc_msgSend)([cls alloc], initialize, 0);
   CHECK(device);
   void *air = dlopen("/annibale/lib/libannibaleair.dylib", RTLD_NOW | RTLD_NOLOAD);
   const char *(*device_name)(void *) = air ? dlsym(air, "rwa_device_name") : NULL;
   SEL mesa = sel_registerName("mesaDevice");
   CHECK(device_name && [device respondsToSelector:mesa]);
   const char *backend = device_name(((void *(*)(id, SEL))objc_msgSend)(device, mesa));
   CHECK(backend && strstr(backend, "softpipe") == NULL);
   printf("reflection pipeline: backend=%s entry=%s\n", backend, function_name);
   NSData *data = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:path]];
   CHECK(data.length);
   dispatch_data_t wrapped = dispatch_data_create(data.bytes, data.length, NULL,
                                                  DISPATCH_DATA_DESTRUCTOR_DEFAULT);
   NSError *error = nil;
   id<MTLLibrary> library = [device newLibraryWithData:wrapped error:&error];
   dispatch_release(wrapped);
   if (!library) {
      fprintf(stderr, "reflection library: %s\n", error.localizedDescription.UTF8String);
      return 1;
   }
   NSString *entry = [NSString stringWithUTF8String:function_name];
   id<MTLFunction> function = [library newFunctionWithName:entry];
   CHECK(function);
   MTLComputePipelineReflection *reflection = nil;
   id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function
      options:MTLPipelineOptionArgumentInfo | MTLPipelineOptionBufferTypeInfo
      reflection:&reflection error:&error];
   if (!pipeline || !reflection) {
      fprintf(stderr, "reflection pipeline: %s\n", error.localizedDescription.UTF8String);
      return 1;
   }
   CHECK(pipeline.maxTotalThreadsPerThreadgroup && pipeline.threadExecutionWidth);
   CHECK(!slm_declarations(reflection, entry));
   printf("PASS original SLM pipeline: %s arguments=%lu bindings=%lu maxThreads=%lu simd=%lu\n",
          function_name, (unsigned long)reflection.arguments.count,
          (unsigned long)reflection.bindings.count,
          (unsigned long)pipeline.maxTotalThreadsPerThreadgroup,
          (unsigned long)pipeline.threadExecutionWidth);
   [pipeline release];
   [function release];
   [library release];
   [device release];
   return 0;
}

int
main(int argc, char **argv)
{
   if (argc != 1 && argc != 3) {
      fprintf(stderr, "usage: metal_reflection_test [CCPortrait.metallib SLM_FUNCTION]\n");
      return 2;
   }
   if (argc == 3) {
      int console = open("/dev/console", O_WRONLY | O_NOCTTY);
      if (console >= 0) {
         dup2(console, STDOUT_FILENO);
         dup2(console, STDERR_FILENO);
         if (console > STDERR_FILENO)
            close(console);
      }
   }
   @autoreleasepool {
      int result = constructor_checks();
      if (result || argc == 1)
         return result;
      return pipeline_check(argv[1], argv[2]);
   }
}
