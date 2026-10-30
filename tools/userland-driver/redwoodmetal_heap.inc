/*
 * MTLHeap without suballocation: every resource made from a heap is its own allocation, as one
 * made by the device is, and the heap keeps the books Metal's callers read back.
 *
 * RenderBox keeps its intermediate textures in automatic heaps (in iconservicesagent, SpringBoard
 * and backboardd). Given nil it makes plain textures instead and logs "unable to create heap: 0",
 * thousands of times a boot, and with a zero size answer it cannot size the heap at all.
 * Separate allocations keep every contract a caller can rely on: an automatic heap promises
 * nothing about placement, and the memory of a placement heap is only reused after
 * -makeAliasable or at an offset that overlaps, where the old contents are undefined anyway.
 * What it costs is memory, the aliased bytes counted twice.
 *
 * Sizes: -size is what the descriptor asked for. -usedSize grows by the size and alignment
 * answer of each live resource and shrinks when it is freed (a slot object associated with the
 * resource), and -maxAvailableSizeWithAlignment: is what is left of -size. Nothing is refused
 * for lack of room, because the room is not real and a caller's own arithmetic, which used our
 * size answers, already decided that it fits.
 */

@interface RedwoodMetalHeap : _MTLObjectWithLabel {
	RedwoodMetalDevice *_owner;
	NSUInteger _size, _storageMode, _cpuCacheMode, _hazardTrackingMode, _type;
	volatile NSUInteger _used;
}
- (instancetype)initWithRedwoodDevice:(RedwoodMetalDevice *)device descriptor:(MTLHeapDescriptor *)descriptor;
- (void)redwoodGive:(NSUInteger)size;
@end

/* What a resource made from a heap carries, as an associated object: the heap (retained, so
 * it outlives its resources as Metal's does), the offset it was asked at and the size it
 * counts for. Freed with the resource, and that is what gives the size back. */
@interface RedwoodMetalHeapSlot : NSObject {
@public
	RedwoodMetalHeap *heap;
	NSUInteger offset, size;
}
@end

@implementation RedwoodMetalHeapSlot
- (void)dealloc
{
	[heap redwoodGive:size];
	[heap release];
	[super dealloc];
}
@end

static const char rw_heap_slot_key;

static RedwoodMetalHeapSlot *rw_heap_slot(id resource)
{
	return objc_getAssociatedObject(resource, &rw_heap_slot_key);
}

static id rw_resource_heap(id resource)
{
	return rw_heap_slot(resource) ? rw_heap_slot(resource)->heap : nil;
}

static NSUInteger rw_resource_heap_offset(id resource)
{
	return rw_heap_slot(resource) ? rw_heap_slot(resource)->offset : 0;
}

/* Bytes a texel, or a block's share of one, by the numbering of MTLPixelFormat. Compressed
 * formats count one byte a texel, at least what any of them takes. */
static NSUInteger rw_heap_texel_bytes(NSUInteger f)
{
	if (f < 20) return 1;                         /* A8, R8 */
	if (f < 50) return 2;                         /* R16, RG8, the 16-bit packed formats */
	if (f < 100) return 4;                        /* R32, RG16, RGBA8, BGRA8, RGB10A2, ... */
	if (f < 120) return 8;                        /* RG32, RGBA16 */
	if (f < 130) return 16;                       /* RGBA32 */
	if (f == 240 || f == 241 || f == 250) return 2;
	if (f < 250 || f == 253) return 1;
	if (f == 260 || f == 261 || f == 552 || f == 553) return 8;
	return 4;
}

static MTLSizeAndAlign rw_heap_texture_size(MTLTextureDescriptor *d)
{
	NSUInteger bpp = rw_heap_texel_bytes([d pixelFormat]), total = 0;
	NSUInteger w = MAX([d width], 1), h = MAX([d height], 1), z = MAX([d depth], 1);
	NSUInteger layers = MAX([d arrayLength], 1) * MAX([d sampleCount], 1);

	if ([d textureType] == MTLTextureTypeCube || [d textureType] == MTLTextureTypeCubeArray)
		layers *= 6;
	for (NSUInteger level = 0; level < MAX([d mipmapLevelCount], 1); level++) {
		NSUInteger row = (MAX(w >> level, 1) * bpp + 63) & ~(NSUInteger)63;
		total += row * MAX(h >> level, 1) * MAX(z >> level, 1);
	}
	total *= layers;
	return (MTLSizeAndAlign){ (total + 65535) & ~(NSUInteger)65535, 65536 };
}

static MTLSizeAndAlign rw_heap_buffer_size(NSUInteger length)
{
	return (MTLSizeAndAlign){ (MAX(length, 1) + 255) & ~(NSUInteger)255, 256 };
}

@implementation RedwoodMetalHeap

- (instancetype)initWithRedwoodDevice:(RedwoodMetalDevice *)device descriptor:(MTLHeapDescriptor *)descriptor
{
	if ((self = [super init]) == nil) return nil;
	_owner = [device retain];
	_size = [descriptor size];
	_storageMode = [descriptor storageMode];
	_cpuCacheMode = [descriptor cpuCacheMode];
	_hazardTrackingMode = [descriptor hazardTrackingMode];
	_type = [descriptor type];
	return self;
}

- (void)dealloc
{
	[_owner release];
	[super dealloc];
}

- (void)redwoodGive:(NSUInteger)size { __sync_fetch_and_sub(&_used, size); }

/* The resource, or nil, with its slot attached and its size counted. */
- (id)redwoodTake:(id)resource size:(NSUInteger)size offset:(NSUInteger)offset
{
	if (resource == nil) return nil;
	RedwoodMetalHeapSlot *slot = [[RedwoodMetalHeapSlot alloc] init];

	slot->heap = [self retain];
	slot->offset = offset;
	slot->size = size;
	__sync_fetch_and_add(&_used, size);
	objc_setAssociatedObject(resource, &rw_heap_slot_key, slot, OBJC_ASSOCIATION_RETAIN);
	[slot release];
	return resource;
}

- (id)device                         { return _owner; }
- (NSUInteger)size                   { return _size; }
- (NSUInteger)usedSize               { return MIN(_used, _size); }
- (NSUInteger)currentAllocatedSize   { return _size; }
- (NSUInteger)storageMode            { return _storageMode; }
- (NSUInteger)cpuCacheMode           { return _cpuCacheMode; }
- (NSUInteger)hazardTrackingMode     { return _hazardTrackingMode; }
- (NSUInteger)type                   { return _type; }
- (NSUInteger)resourceOptions
{
	return (_cpuCacheMode << MTLResourceCPUCacheModeShift) |
	       (_storageMode << MTLResourceStorageModeShift) |
	       (_hazardTrackingMode << MTLResourceHazardTrackingModeShift);
}

- (NSUInteger)maxAvailableSizeWithAlignment:(NSUInteger)alignment
{
	NSUInteger left = _size - [self usedSize];

	return alignment > 1 ? left & ~(alignment - 1) : left;
}

/* Nothing here is ever volatile (see the buffer's own answer), so NonVolatile. */
- (NSUInteger)setPurgeableState:(NSUInteger)state { (void)state; return 2; }

- (id)newBufferWithLength:(NSUInteger)length options:(NSUInteger)options
{
	return [self redwoodTake:[_owner newBufferWithLength:length options:options]
			    size:rw_heap_buffer_size(length).size offset:0];
}

- (id)newBufferWithLength:(NSUInteger)length options:(NSUInteger)options offset:(NSUInteger)offset
{
	return [self redwoodTake:[_owner newBufferWithLength:length options:options]
			    size:rw_heap_buffer_size(length).size offset:offset];
}

- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor
{
	return [self redwoodTake:[_owner newTextureWithDescriptor:descriptor]
			    size:rw_heap_texture_size(descriptor).size offset:0];
}

- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor offset:(NSUInteger)offset
{
	return [self redwoodTake:[_owner newTextureWithDescriptor:descriptor]
			    size:rw_heap_texture_size(descriptor).size offset:offset];
}

@end

@implementation RedwoodMetalDevice (Heap)

- (id)newHeapWithDescriptor:(MTLHeapDescriptor *)descriptor
{
	static unsigned said;

	if (__sync_fetch_and_add(&said, 1) < 16)
		rw_print("heap: type %lu, %lu bytes, storage %lu, hazard tracking %lu\n",
			 (unsigned long)[descriptor type], (unsigned long)[descriptor size],
			 (unsigned long)[descriptor storageMode],
			 (unsigned long)[descriptor hazardTrackingMode]);
	return [[RedwoodMetalHeap alloc] initWithRedwoodDevice:self descriptor:descriptor];
}

- (MTLSizeAndAlign)heapBufferSizeAndAlignWithLength:(NSUInteger)length options:(NSUInteger)options
{
	(void)options;
	return rw_heap_buffer_size(length);
}

- (MTLSizeAndAlign)heapTextureSizeAndAlignWithDescriptor:(MTLTextureDescriptor *)descriptor
{
	return rw_heap_texture_size(descriptor);
}

@end
