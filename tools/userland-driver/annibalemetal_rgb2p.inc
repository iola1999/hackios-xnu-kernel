/* RGB2P stores {Cr,Y,Cb} in the surface's Y and CbCr planes without a colour matrix. 542 ('_RB4',
 * RenderBox's opaque layers) keeps both planes full size: plane 0 G, plane 1 B and R. */
static id
rw_combined_rgb2p(AnnibaleMetalDevice *device, MTLTextureDescriptor *d, void *surface,
		NSUInteger plane, const struct rwa_texture_shape *shape)
{
	uint32_t (*get_format)(void *) = dlsym(RTLD_DEFAULT, "IOSurfaceGetPixelFormat");
	size_t (*get_width)(void *, size_t) = dlsym(RTLD_DEFAULT, "IOSurfaceGetWidthOfPlane");
	size_t (*get_height)(void *, size_t) = dlsym(RTLD_DEFAULT, "IOSurfaceGetHeightOfPlane");
	size_t (*get_count)(void *) = dlsym(RTLD_DEFAULT, "IOSurfaceGetPlaneCount");
	bool ten = shape->pixel_format == 543 || shape->pixel_format == 544;
	bool full = shape->pixel_format == 542;
	unsigned horizontal = full ? 1 : 2, vertical = full || shape->pixel_format == 544 ? 1 : 2;
	unsigned effective = shape->usage ? shape->usage : 5;
	bool render = (effective & MTLTextureUsageRenderTarget) != 0;
	if (!surface || plane || !get_format || !get_width || !get_height || !get_count ||
	    !mesa.texture_combine_rgb2p || (render && !mesa.texture_rgb2p_pass) ||
	    get_count(surface) != 2 || (effective & ~0x15u) || shape->type != 2 ||
	    shape->mip_levels != 1 || shape->sample_count != 1 || shape->depth != 1 ||
	    shape->array_length != 1 || !shape->width || !shape->height ||
	    (!full && ((shape->width & 1) || (shape->height & 1))) ||
	    shape->width > get_width(surface, 0) || shape->height > get_height(surface, 0) ||
	    (shape->width + horizontal - 1)/horizontal > get_width(surface, 1) ||
	    (shape->height + vertical - 1)/vertical > get_height(surface, 1)) {
		rw_print("RGB2P requires a two-plane, single-level sample/render surface: plane %lu of %zu, format %u "
		    "type %u %ux%u depth %u array %u mips %u samples %u usage %#x, planes %zux%zu and %zux%zu\n",
		    (unsigned long)plane, surface && get_count ? get_count(surface) : 0, shape->pixel_format,
		    shape->type, shape->width, shape->height, shape->depth, shape->array_length, shape->mip_levels,
		    shape->sample_count, effective, surface && get_width ? get_width(surface, 0) : 0,
		    surface && get_height ? get_height(surface, 0) : 0, surface && get_width ? get_width(surface, 1) : 0,
		    surface && get_height ? get_height(surface, 1) : 0);
		return nil;
	}
	uint32_t format = get_format(surface);
	bool valid = full ? format == '_RB4' : !ten ? (format == '420v' || format == '420f') : vertical == 2
	    ? (format == 'x420' || format == 'xf20') : (format == 'x422' || format == 'xf22');
	if (!valid) {
		rw_print("RGB2P format %u does not match surface FourCC %#x\n", shape->pixel_format, format);
		return nil;
	}
	id planes[2] = {nil, nil};
	for (unsigned p = 0; p < 2; p++) {
		MTLTextureDescriptor *part = [d copy];
		part.pixelFormat = ten ? (p ? MTLPixelFormatRG16Unorm : MTLPixelFormatR16Unorm)
		                       : (p ? MTLPixelFormatRG8Unorm : MTLPixelFormatR8Unorm);
		part.width = p ? (shape->width + horizontal - 1)/horizontal : shape->width;
		part.height = p ? (shape->height + vertical - 1)/vertical : shape->height;
		part.usage = MTLTextureUsageShaderRead | (render ? MTLTextureUsageShaderWrite : 0);
		planes[p] = [(id<MTLDevice>)device newTextureWithDescriptor:part iosurface:surface plane:p];
		[part release];
		if (!planes[p]) {
			[planes[0] release];
			return nil;
		}
	}
	char why[256] = {0};
	struct rwa_texture *combined = mesa.texture_combine_rgb2p([planes[0] mesaTexture],
	    [planes[1] mesaTexture], shape->pixel_format, shape->usage, why, sizeof(why));
	AnnibaleMetalYUVTexture *texture = nil;
	if (combined) {
		texture = [[AnnibaleMetalYUVTexture alloc] initWithAnnibaleDevice:device
		    texture:combined shape:shape iosurface:surface plane:0];
		[texture annibaleSetPlanes:@[planes[0], planes[1]]];
	} else {
		rw_print("combined RGB2P: %s\n", why);
	}
	[planes[0] release];
	[planes[1] release];
	return texture;
}
