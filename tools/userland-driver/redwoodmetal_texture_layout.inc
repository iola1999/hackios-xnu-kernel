/*
 * Images kept in memory laid out for the GPU: -newTextureLayoutWithDescriptor: and the textures
 * -newTextureWithBytesNoCopy: makes over such memory.
 *
 * ImageIO decodes ATX and KTX images into the memory of a texture layout (initialize, copy from
 * linear bytes, finalize), QuartzCore sends that memory to backboardd as a CompressedImage (a copy
 * memory entry of the layout's size) and backboardd makes the texture with
 * -newTextureWithBytesNoCopy:. With no layout the image's data had a length of 0, SpringBoard's
 * memory entry failed (KERN_INVALID_ARGUMENT) and backboardd refused the whole commit of the
 * window holding the image ("CompressedImage port is MACH_PORT_NULL!"). In bo46 and bo47 that
 * window was SBMainSwitcherWindow, whose wallpaper snapshot is an ATX image and which shows every
 * application's scene: applications opened black.
 *
 * The layout is this driver's own, and only this driver reads it: each level of each slice in rows
 * of texel blocks with no padding, the levels of a slice one after another, then the next slice.
 */

struct rw_block {
	unsigned width, height, bytes;
};

/* Texel block of a pixel format: 1x1 for formats in texels, 4x4 and the ASTC sizes for the
 * compressed ones. NO for formats this layout does not hold (PVRTC, depth, YCbCr, XR). */
static BOOL
rw_format_block(NSUInteger format, struct rw_block *b)
{
	/* ASTC from MTLPixelFormatASTC_4x4_sRGB (186), _LDR (204) and _HDR (222); +5 is unused */
	static const unsigned char astc[15][2] = {
		{ 4, 4 }, { 5, 4 }, { 5, 5 }, { 6, 5 }, { 6, 6 }, { 0, 0 }, { 8, 5 }, { 8, 6 },
		{ 8, 8 }, { 10, 5 }, { 10, 6 }, { 10, 8 }, { 10, 10 }, { 12, 10 }, { 12, 12 },
	};
	static const NSUInteger astc_base[3] = { 186, 204, 222 };

	for (unsigned i = 0; i < 3; i++) {
		if (format >= astc_base[i] && format < astc_base[i] + 15 && astc[format - astc_base[i]][0] != 0) {
			*b = (struct rw_block){ astc[format - astc_base[i]][0], astc[format - astc_base[i]][1], 16 };
			return YES;
		}
	}
	*b = (struct rw_block){ 1, 1, 0 };
	switch (format) {
	case 1: case 10: case 11: case 12: case 13: case 14:
		b->bytes = 1; break;
	case 20: case 22: case 23: case 24: case 25: case 30: case 31: case 32: case 33: case 34:
	case 40: case 41: case 42: case 43:
		b->bytes = 2; break;
	case 53: case 54: case 55: case 60: case 62: case 63: case 64: case 65: case 70: case 71:
	case 72: case 73: case 74: case 80: case 81: case 90: case 91: case 92: case 93: case 94:
		b->bytes = 4; break;
	case 103: case 104: case 105: case 110: case 112: case 113: case 114: case 115:
		b->bytes = 8; break;
	case 123: case 124: case 125:
		b->bytes = 16; break;
	case 130: case 131: case 140: case 141: case 170: case 172: case 180: case 181: case 182: case 183:
		*b = (struct rw_block){ 4, 4, 8 }; break;          /* BC1, BC4, EAC R11, ETC2 RGB8(A1) */
	case 132: case 133: case 134: case 135: case 142: case 143: case 150: case 151: case 152: case 153:
	case 174: case 176: case 178: case 179:
		*b = (struct rw_block){ 4, 4, 16 }; break;         /* BC2, BC3, BC5, BC6H, BC7, EAC RG11, EAC RGBA8 */
	default:
		break;
	}
	return b->bytes != 0;
}

@interface RedwoodTextureLayout : NSObject {
@public
	struct rw_block _block;
	NSUInteger _width, _height, _depth, _levels, _slices, _size;
}
@end

static NSUInteger rw_layout_extent(NSUInteger n, NSUInteger level) { return n >> level ? n >> level : 1; }

static NSUInteger
rw_layout_row_bytes(const RedwoodTextureLayout *l, NSUInteger level)
{
	return (rw_layout_extent(l->_width, level) + l->_block.width - 1) / l->_block.width * l->_block.bytes;
}

static NSUInteger
rw_layout_image_bytes(const RedwoodTextureLayout *l, NSUInteger level)
{
	return rw_layout_row_bytes(l, level) *
	       ((rw_layout_extent(l->_height, level) + l->_block.height - 1) / l->_block.height);
}

static NSUInteger
rw_layout_offset(const RedwoodTextureLayout *l, NSUInteger slice, NSUInteger level)
{
	NSUInteger slice_bytes = 0, at = 0;

	for (NSUInteger k = 0; k < l->_levels; k++) {
		NSUInteger bytes = rw_layout_image_bytes(l, k) * rw_layout_extent(l->_depth, k);

		slice_bytes += bytes;
		at += k < level ? bytes : 0;
	}
	return slice * slice_bytes + at;
}

/* Copies a region of one level and slice between the layout's memory and linear bytes. NO, and
 * nothing copied, when the region does not lie on whole blocks inside the level. */
static BOOL
rw_layout_copy(const RedwoodTextureLayout *l, uint8_t *memory, uint8_t *linear, NSUInteger row,
	       NSUInteger image, NSUInteger slice, NSUInteger level, MTLRegion r, BOOL to_memory)
{
	const struct rw_block *b = &l->_block;
	NSUInteger row_bytes = rw_layout_row_bytes(l, level), image_bytes = rw_layout_image_bytes(l, level);
	NSUInteger x = r.origin.x / b->width * b->bytes, y = r.origin.y / b->height;
	NSUInteger bytes = (r.size.width + b->width - 1) / b->width * b->bytes;
	NSUInteger rows = (r.size.height + b->height - 1) / b->height;

	if (memory == NULL || linear == NULL || slice >= l->_slices || level >= l->_levels ||
	    r.origin.x % b->width || r.origin.y % b->height || x + bytes > row_bytes ||
	    (y + rows) * row_bytes > image_bytes ||
	    r.origin.z + r.size.depth > rw_layout_extent(l->_depth, level)) {
		return NO;
	}
	uint8_t *base = memory + rw_layout_offset(l, slice, level);
	for (NSUInteger z = 0; z < r.size.depth; z++) {
		for (NSUInteger k = 0; k < rows; k++) {
			uint8_t *m = base + (r.origin.z + z) * image_bytes + (y + k) * row_bytes + x;
			uint8_t *s = linear + z * image + k * row;

			memcpy(to_memory ? m : s, to_memory ? s : m, bytes);
		}
	}
	return YES;
}

static RedwoodTextureLayout *
rw_texture_layout_new(MTLTextureDescriptor *d)
{
	struct rw_block block;
	NSUInteger type = [d textureType];

	if (d == nil || [d sampleCount] > 1 || !rw_format_block([d pixelFormat], &block) || [d width] == 0) {
		return nil;
	}
	RedwoodTextureLayout *l = [RedwoodTextureLayout new];
	l->_block = block;
	l->_width = [d width];
	l->_height = [d height] ? [d height] : 1;
	l->_depth = type == MTLTextureType3D && [d depth] ? [d depth] : 1;
	l->_levels = [d mipmapLevelCount] ? [d mipmapLevelCount] : 1;
	l->_slices = ([d arrayLength] ? [d arrayLength] : 1) *
		     (type == MTLTextureTypeCube || type == MTLTextureTypeCubeArray ? 6 : 1);
	l->_size = rw_layout_offset(l, l->_slices, 0);
	return l;
}

@implementation RedwoodTextureLayout
- (unsigned long long)size { return _size; }
- (unsigned long long)alignment { return 256; }
- (MTLSize)copyGranularity { return MTLSizeMake(_block.width, _block.height, 1); }
- (void)initializeTextureMemory:(void *)memory { if (memory != NULL) memset(memory, 0, _size); }
- (void)finalizeTextureMemory:(void *)memory { (void)memory; }

- (void)copyFromLinearBytes:(const void *)bytes
	       linearOffset:(unsigned long long)offset
	  linearBytesPerRow:(unsigned long long)row
	linearBytesPerImage:(unsigned long long)image
	    toTextureMemory:(void *)memory
	       textureSlice:(unsigned long long)slice
	       textureLevel:(unsigned long long)level
	      textureRegion:(MTLRegion)region
{
	if (!rw_layout_copy(self, memory, bytes ? (uint8_t *)bytes + offset : NULL, row, image, slice, level, region, YES)) {
		RW_ONCE("redwoodmetal: texture layout: a copy into texture memory outside the level was refused\n");
	}
}

- (void)copyFromTextureMemory:(const void *)memory
		 textureSlice:(unsigned long long)slice
		 textureLevel:(unsigned long long)level
		textureRegion:(MTLRegion)region
		toLinearBytes:(void *)bytes
		 linearOffset:(unsigned long long)offset
	    linearBytesPerRow:(unsigned long long)row
	  linearBytesPerImage:(unsigned long long)image
{
	if (!rw_layout_copy(self, (uint8_t *)memory, bytes ? (uint8_t *)bytes + offset : NULL, row, image, slice, level, region, NO)) {
		RW_ONCE("redwoodmetal: texture layout: a copy out of texture memory outside the level was refused\n");
	}
}

/* A selector of the layout protocol this class does not implement: one line naming it, and zero. */
- (NSMethodSignature *)methodSignatureForSelector:(SEL)selector
{
	NSMethodSignature *signature = [super methodSignatureForSelector:selector];
	struct objc_method_description description;

	if (signature == nil && protocol_declares("MTLTextureLayout", selector, &description)) {
		signature = [NSMethodSignature signatureWithObjCTypes:description.types];
	}
	return signature;
}

- (void)forwardInvocation:(NSInvocation *)invocation
{
	static const char none[32];
	NSUInteger length = invocation.methodSignature.methodReturnLength;

	rw_print("texture layout selector %s is not implemented\n", sel_getName(invocation.selector));
	if (length > 0 && length <= sizeof(none)) {
		[invocation setReturnValue:(void *)none];
	}
}
@end

@implementation RedwoodMetalDevice (TextureLayout)

- (id)newTextureLayoutWithDescriptor:(MTLTextureDescriptor *)descriptor isHeapOrBufferBacked:(BOOL)backed
{
	(void)backed;
	RedwoodTextureLayout *layout = rw_texture_layout_new(descriptor);

	if (layout == nil) {
		RW_AGAIN("newTextureLayoutWithDescriptor: no layout for pixel format %lu, %lu samples\n",
			 (unsigned long)[descriptor pixelFormat], (unsigned long)[descriptor sampleCount]);
	}
	return layout;
}

/*
 * A texture over memory in the layout above. The texture is made by copying the memory once, which
 * is right for what reaches here, CompressedImage data that nobody writes after it is sent. A
 * texture the GPU may write, or memory smaller than the layout, is refused: a copy would not follow
 * the caller's later writes. A length of 0 is taken as the whole layout: hakios-arm64e-dada notes
 * that RenderBox passes 0 for its ASTC images.
 */
- (id)newTextureWithBytesNoCopy:(void *)bytes length:(NSUInteger)length
		     descriptor:(MTLTextureDescriptor *)descriptor
		    deallocator:(void (^)(void *, NSUInteger))deallocator
{
	RedwoodTextureLayout *layout = rw_texture_layout_new(descriptor);
	NSUInteger usage = [descriptor usage];
	id texture = nil;

	if (bytes == NULL || layout == nil || (usage & (MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget)) ||
	    (length != 0 && length < layout->_size)) {
		RW_AGAIN("newTextureWithBytesNoCopy: refused: format %lu %lux%lu usage %#lx, %lu bytes for a layout of %lu\n",
			 (unsigned long)[descriptor pixelFormat], (unsigned long)[descriptor width],
			 (unsigned long)[descriptor height], (unsigned long)usage, (unsigned long)length,
			 (unsigned long)(layout ? layout->_size : 0));
		[layout release];
		return nil;
	}
	texture = [self newTextureWithDescriptor:descriptor];
	for (NSUInteger slice = 0; texture != nil && slice < layout->_slices; slice++) {
		for (NSUInteger level = 0; level < layout->_levels; level++) {
			MTLRegion region = MTLRegionMake3D(0, 0, 0, rw_layout_extent(layout->_width, level),
							   rw_layout_extent(layout->_height, level),
							   rw_layout_extent(layout->_depth, level));

			[texture replaceRegion:region mipmapLevel:level slice:slice
				     withBytes:(const uint8_t *)bytes + rw_layout_offset(layout, slice, level)
				   bytesPerRow:rw_layout_row_bytes(layout, level)
				 bytesPerImage:rw_layout_image_bytes(layout, level)];
		}
	}
	if (texture != nil && deallocator != nil) {
		static char owner_key;
		NSData *owner = [[NSData alloc] initWithBytesNoCopy:bytes length:length ? length : layout->_size
							deallocator:deallocator];

		objc_setAssociatedObject(texture, &owner_key, owner, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
		[owner release];
	}
	RW_ONCE("redwoodmetal: first texture over caller memory: format %lu %lux%lu, %lu levels, %lu bytes\n",
		(unsigned long)[descriptor pixelFormat], (unsigned long)[descriptor width],
		(unsigned long)[descriptor height], (unsigned long)layout->_levels, (unsigned long)layout->_size);
	[layout release];
	return texture;
}

@end
