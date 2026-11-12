/* Copyright © 2026 Darwin on annibale contributors
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdlib.h>

/* Metal 26's native binding and type constructors take public enum values.
 * Struct setters with doRetain: replace the binding's pointer type; their
 * single-argument variants are diagnostic stubs in the iOS shared cache.
 */
@interface NSObject (AnnibaleMetalReflectionSPI)
- (id)initWithName:(NSString *)name access:(NSUInteger)access isActive:(BOOL)active
     locationIndex:(NSUInteger)index arraySize:(NSUInteger)length
          dataType:(NSUInteger)type textureType:(NSUInteger)textureType
    isDepthTexture:(BOOL)depth;
- (id)initWithName:(NSString *)name type:(NSUInteger)type access:(NSUInteger)access
          isActive:(BOOL)active locationIndex:(NSUInteger)index
         arraySize:(NSUInteger)length dataType:(NSUInteger)dataType
       pixelFormat:(NSUInteger)format aluType:(NSUInteger)aluType
  isConstantBuffer:(BOOL)constant dataSize:(NSUInteger)size alignment:(NSUInteger)alignment;
- (id)initWithName:(NSString *)name type:(NSUInteger)type access:(NSUInteger)access
          isActive:(BOOL)active locationIndex:(NSUInteger)index
         arraySize:(NSUInteger)length dataType:(NSUInteger)dataType
          dataSize:(NSUInteger)size alignment:(NSUInteger)alignment;
- (id)initWithName:(NSString *)name type:(NSUInteger)type access:(NSUInteger)access
             index:(NSUInteger)index active:(BOOL)active arrayLength:(NSUInteger)length;
- (void)setIsArgument:(BOOL)isArgument;
- (void)setStructType:(id)type doRetain:(BOOL)retain;
- (id)initWithMembers:(id *)members count:(NSUInteger)count typeName:(NSString *)name;
- (id)initWithName:(NSString *)name offset:(NSUInteger)offset dataType:(NSUInteger)type
       pixelFormat:(NSUInteger)format aluType:(NSUInteger)aluType
 indirectArgumentIndex:(NSUInteger)argumentIndex render_target:(NSUInteger)target
 raster_order_group:(NSUInteger)group details:(id)details;
- (id)initWithArrayLength:(NSUInteger)length elementType:(NSUInteger)element
                  stride:(NSUInteger)stride pixelFormat:(NSUInteger)format
                 aluType:(NSUInteger)aluType details:(id)details;
@end

@interface AnnibaleMetalComputeReflection : MTLComputePipelineReflection {
   NSArray *_bindings;
}
- (id)initWithBindings:(NSArray *)bindings;
@end

@implementation AnnibaleMetalComputeReflection
- (id)initWithBindings:(NSArray *)bindings
{
   self = [super init];
   if (self)
      _bindings = [bindings copy];
   return self;
}
- (NSArray *)bindings { return _bindings; }
- (NSArray *)arguments { return _bindings; }
- (void)dealloc
{
   [_bindings release];
   [super dealloc];
}
@end

static id
rw_reflection_alloc(const char *name, SEL initializer, NSString **why)
{
   Class type = objc_getClass(name);
   if (type == Nil || class_getInstanceMethod(type, initializer) == NULL) {
      *why = [NSString stringWithFormat:@"Metal reflection class %s lacks %s", name,
                                        sel_getName(initializer)];
      return nil;
   }
   return [type alloc];
}

static NSString *
rw_reflection_name(const char *name, NSString **why)
{
   NSString *result = name ? [NSString stringWithUTF8String:name] : nil;
   if (!result)
      *why = @"Metal reflection name is missing or contains invalid UTF-8";
   return result;
}

static id
rw_reflection_type(const struct rwa_reflection_type *type, NSString **why)
{
   if (type->data_type == MTLDataTypeStruct) {
      NSString *typeName = rw_reflection_name(type->name, why);
      if (!typeName)
         return nil;
      NSMutableArray *members = [NSMutableArray arrayWithCapacity:type->member_count];
      for (unsigned i = 0; i < type->member_count; i++) {
         const struct rwa_reflection_member *field = &type->members[i];
         NSString *fieldName = rw_reflection_name(field->name, why);
         if (!fieldName)
            return nil;
         id details = rw_reflection_type(field->type, why);
         if (*why)
            return nil;
         SEL selector = @selector(initWithName:offset:dataType:pixelFormat:aluType:indirectArgumentIndex:render_target:raster_order_group:details:);
         id member = rw_reflection_alloc("MTLStructMemberInternal", selector, why);
         if (member)
            member = [member initWithName:fieldName
                                  offset:field->offset dataType:field->type->data_type
                             pixelFormat:0 aluType:0 indirectArgumentIndex:0
                           render_target:UINT32_MAX raster_order_group:UINT32_MAX
                                 details:details];
         [details release];
         if (member == nil) {
            if (!*why)
               *why = @"Metal could not construct a struct member";
            return nil;
         }
         [members addObject:member];
         [member release];
      }
      id *objects = calloc(type->member_count, sizeof(*objects));
      if (type->member_count && objects == NULL) {
         *why = @"out of memory constructing Metal struct reflection";
         return nil;
      }
      [members getObjects:objects range:NSMakeRange(0, type->member_count)];
      id result = rw_reflection_alloc("MTLStructTypeInternal",
                                      @selector(initWithMembers:count:typeName:), why);
      if (result)
         result = [result initWithMembers:objects count:type->member_count
                                typeName:typeName];
      free(objects);
      if (!result && !*why)
         *why = @"Metal could not construct a struct type";
      return result;
   }
   if (type->data_type == MTLDataTypeArray) {
      id details = rw_reflection_type(type->element, why);
      if (*why)
         return nil;
      id result = rw_reflection_alloc("MTLArrayTypeInternal",
         @selector(initWithArrayLength:elementType:stride:pixelFormat:aluType:details:), why);
      if (result)
         result = [result initWithArrayLength:type->array_length
                                 elementType:type->element->data_type
                                      stride:type->array_stride pixelFormat:0 aluType:0
                                     details:details];
      [details release];
      if (!result && !*why)
         *why = @"Metal could not construct an array type";
      return result;
   }
   if (!((type->data_type >= MTLDataTypeFloat && type->data_type <= MTLDataTypeBool4) ||
         (type->data_type >= MTLDataTypeLong && type->data_type <= MTLDataTypeULong4)))
      *why = @"unsupported Metal reflection data type";
   return nil;
}

static id
rw_reflection_argument(const struct rwa_reflection_argument *arg, NSString **why)
{
   NSString *name = rw_reflection_name(arg->name, why);
   if (!name)
      return nil;
   id result = nil;
   if (arg->type == MTLBindingTypeTexture) {
      result = rw_reflection_alloc("MTLTextureBindingInternal",
         @selector(initWithName:access:isActive:locationIndex:arraySize:dataType:textureType:isDepthTexture:), why);
      if (result)
         result = [result initWithName:name access:arg->access isActive:arg->active
                        locationIndex:arg->index arraySize:arg->array_length
                             dataType:arg->texture_data_type textureType:arg->texture_type
                       isDepthTexture:arg->depth_texture];
   } else if (arg->type == MTLBindingTypeSampler) {
      result = rw_reflection_alloc("MTLBindingInternal",
         @selector(initWithName:type:access:index:active:arrayLength:), why);
      if (result)
         result = [result initWithName:name type:arg->type access:arg->access
                                index:arg->index active:arg->active
                          arrayLength:arg->array_length];
   } else if (arg->type == MTLBindingTypeBuffer ||
              arg->type == MTLBindingTypeThreadgroupMemory) {
      const struct rwa_reflection_type *type = arg->buffer_type;
      if (!type || type->data_type == MTLDataTypeArray) {
         *why = @"Metal buffer reflection requires a scalar or struct pointee";
         return nil;
      }
      if (type->alignment > UINT16_MAX ||
          (arg->type == MTLBindingTypeBuffer && type->data_size == UINT32_MAX)) {
         *why = @"Metal buffer reflection size or alignment exceeds the native representation";
         return nil;
      }
      /* GraphSPI constant inputs have an opaque pointee in native reflection. */
      BOOL opaque = arg->type == MTLBindingTypeBuffer && arg->constant_buffer &&
                    type->data_type == MTLDataTypeNone && type->data_size == 0;
      id details = opaque ? nil : rw_reflection_type(type, why);
      if (*why)
         return nil;
      if (arg->type == MTLBindingTypeBuffer) {
         result = rw_reflection_alloc("MTLBufferBindingInternal",
            @selector(initWithName:type:access:isActive:locationIndex:arraySize:dataType:pixelFormat:aluType:isConstantBuffer:dataSize:alignment:), why);
         if (result)
            result = [result initWithName:name type:arg->type access:arg->access
                                isActive:arg->active locationIndex:arg->index
                               arraySize:arg->array_length dataType:type->data_type
                             pixelFormat:0 aluType:0 isConstantBuffer:arg->constant_buffer
                                dataSize:type->data_size alignment:type->alignment];
      } else {
         result = rw_reflection_alloc("MTLThreadgroupMemoryBindingInternal",
            @selector(initWithName:type:access:isActive:locationIndex:arraySize:dataType:dataSize:alignment:), why);
         if (result)
            result = [result initWithName:name type:arg->type access:arg->access
                                isActive:arg->active locationIndex:arg->index
                               arraySize:arg->array_length dataType:type->data_type
                                dataSize:type->data_size alignment:type->alignment];
      }
      if (result && type->data_type == MTLDataTypeStruct) {
         if (![result respondsToSelector:@selector(setStructType:doRetain:)]) {
            *why = @"Metal buffer binding lacks the struct type setter";
            [result release];
            result = nil;
         } else {
            [result setStructType:details doRetain:YES];
         }
      }
      [details release];
   } else {
      *why = @"unsupported Metal reflection binding type";
      return nil;
   }
   if (result == nil) {
      if (!*why)
         *why = @"Metal could not construct an argument binding";
      return nil;
   }
   if (![result respondsToSelector:@selector(setIsArgument:)]) {
      *why = @"Metal binding lacks setIsArgument:";
      [result release];
      return nil;
   }
   [result setIsArgument:YES];
   return result;
}

static id
rw_compute_reflection(const struct rwa_compute_reflection *metadata, NSString **why)
{
   *why = nil;
   NSMutableArray *arguments = [NSMutableArray arrayWithCapacity:metadata->argument_count];
   for (unsigned i = 0; i < metadata->argument_count; i++) {
      id argument = rw_reflection_argument(&metadata->arguments[i], why);
      if (!argument)
         return nil;
      [arguments addObject:argument];
      [argument release];
   }
   return [[AnnibaleMetalComputeReflection alloc] initWithBindings:arguments];
}
