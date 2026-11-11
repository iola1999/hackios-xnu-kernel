/* Combined NV12 textures own both Metal plane wrappers, including copied buffers. */
@interface AnnibaleMetalYUVTexture : AnnibaleMetalTexture {
	NSArray *_planes;
}
- (void)annibaleSetPlanes:(NSArray *)planes;
@end

@implementation AnnibaleMetalYUVTexture
- (void)annibaleSetPlanes:(NSArray *)planes { _planes = [planes retain]; }
- (void)dealloc
{
	/* AIR retains the plane resources; the wrappers retain their copied buffers. */
	[_planes release];
	[super dealloc];
}
@end

static id
rw_combined_yuv(AnnibaleMetalDevice *device, MTLTextureDescriptor *d, void *surface,
		NSUInteger plane, const struct rwa_texture_shape *shape)
{
	char why[256] = { 0 };
	unsigned matrix = 0;
	uint32_t (*get_format)(void *) = dlsym(RTLD_DEFAULT, "IOSurfaceGetPixelFormat");
	size_t (*get_width)(void *, size_t) = dlsym(RTLD_DEFAULT, "IOSurfaceGetWidthOfPlane");
	size_t (*get_height)(void *, size_t) = dlsym(RTLD_DEFAULT, "IOSurfaceGetHeightOfPlane");
	size_t (*get_count)(void *) = dlsym(RTLD_DEFAULT, "IOSurfaceGetPlaneCount");
	int (*get_matrix)(void *, unsigned *) = dlsym(RTLD_DEFAULT, "IOSurfaceGetYCbCrMatrix");
	bool ten_bit = shape->pixel_format == 505;
	if (!surface || plane != 0 || !get_format || !get_width || !get_height || !get_count ||
	    !get_matrix || !mesa.texture_combine_yuv || get_count(surface) != 2 ||
	    (shape->usage & ~0x15u) || shape->type != 2 || shape->mip_levels != 1 ||
	    shape->sample_count != 1 || shape->depth != 1 || shape->array_length != 1 ||
	    shape->width == 0 || shape->height == 0 ||
	    shape->width > get_width(surface, 0) || shape->height > get_height(surface, 0)) {
		rw_print("combined YCbCr requires a two-plane, single-level sampled surface\n");
		return nil;
	}
	uint32_t format = get_format(surface);
	bool valid_format = ten_bit ? (format == 0x78343230 || format == 0x78663230)
	                            : (format == 0x34323076 || format == 0x34323066);
	if (!valid_format || get_matrix(surface, &matrix) ||
	    (matrix != 0 && matrix != 1 && matrix != 5 && matrix != 6 && matrix != 9 && matrix != 10)) {
		rw_print("combined YCbCr unsupported surface format %#x matrix %u\n", format, matrix);
		return nil;
	}
	/* Native Metal uses BT.601 for an unspecified IOSurface matrix; 5 is BT.601 PAL/BG, 10 is BT.2020 CL. */
	if (matrix == 0 || matrix == 5)
		matrix = 6;
	else if (matrix == 10)
		matrix = 9;
	id planes[2] = { nil, nil };
	for (unsigned p = 0; p < 2; p++) {
		MTLTextureDescriptor *part = [d copy];
		part.pixelFormat = ten_bit ? (p ? MTLPixelFormatRG16Unorm : MTLPixelFormatR16Unorm)
		                           : (p ? MTLPixelFormatRG8Unorm : MTLPixelFormatR8Unorm);
		part.width = p ? (shape->width + 1) / 2 : shape->width;
		part.height = p ? (shape->height + 1) / 2 : shape->height;
		part.usage = MTLTextureUsageShaderRead;
		planes[p] = [device newTextureWithDescriptor:part iosurface:surface plane:p];
		[part release];
		if (!planes[p]) {
			[planes[0] release];
			return nil;
		}
	}
	bool full_range = (format == 0x34323066 || format == 0x78663230);
	struct rwa_texture *combined = mesa.texture_combine_yuv([planes[0] mesaTexture],
		[planes[1] mesaTexture], full_range, matrix, why, sizeof(why));
	AnnibaleMetalYUVTexture *texture = nil;
	if (combined) {
		texture = [[AnnibaleMetalYUVTexture alloc] initWithAnnibaleDevice:device
			texture:combined shape:shape iosurface:surface plane:0];
		[texture annibaleSetPlanes:@[planes[0], planes[1]]];
	} else {
		rw_print("combined NV12: %s\n", why);
	}
	[planes[0] release];
	[planes[1] release];
	return texture;
}
