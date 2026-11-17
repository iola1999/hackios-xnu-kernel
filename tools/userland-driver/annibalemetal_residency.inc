/*
 * MTLResidencySet bookkeeping. Every GEM and Mesa allocation on this driver is
 * resident for its lifetime, so commit/requestResidency/endResidency manage
 * the allocation set and committed byte footprint that CMImaging and Metal 3/4
 * callers query back.
 */

@interface AnnibaleMetalResidencySet : _MTLObjectWithLabel {
	AnnibaleMetalDevice *_owner;
	NSMutableOrderedSet *_allocations;
	uint64_t _committedAllocatedSize;
	pthread_mutex_t _lock;
}
- (instancetype)initWithAnnibaleDevice:(AnnibaleMetalDevice *)device descriptor:(id)descriptor;
@end

@implementation AnnibaleMetalResidencySet

- (instancetype)initWithAnnibaleDevice:(AnnibaleMetalDevice *)device descriptor:(id)descriptor
{
	if ((self = [super init]) == nil)
		return nil;
	pthread_mutex_init(&_lock, NULL);
	_owner = [device retain];
	NSUInteger cap = 0;
	if ([descriptor respondsToSelector:@selector(initialCapacity)])
		cap = [descriptor initialCapacity];
	_allocations = cap ? [[NSMutableOrderedSet alloc] initWithCapacity:cap]
			   : [[NSMutableOrderedSet alloc] init];
	if ([descriptor respondsToSelector:@selector(label)] && [descriptor label] != nil)
		[self setLabel:[descriptor label]];
	return self;
}

- (void)dealloc
{
	[_allocations release];
	[_owner release];
	pthread_mutex_destroy(&_lock);
	[super dealloc];
}

- (id)device { return _owner; }

- (uint64_t)allocatedSize
{
	pthread_mutex_lock(&_lock);
	uint64_t sz = _committedAllocatedSize;
	pthread_mutex_unlock(&_lock);
	return sz;
}

- (void)requestResidency { }
- (void)endResidency { }

- (void)addAllocation:(id)allocation
{
	if (allocation == nil)
		return;
	pthread_mutex_lock(&_lock);
	[_allocations addObject:allocation];
	pthread_mutex_unlock(&_lock);
}

- (void)addAllocations:(const id *)allocations count:(NSUInteger)count
{
	if (allocations == NULL || count == 0)
		return;
	pthread_mutex_lock(&_lock);
	for (NSUInteger i = 0; i < count; i++) {
		if (allocations[i] != nil)
			[_allocations addObject:allocations[i]];
	}
	pthread_mutex_unlock(&_lock);
}

- (void)removeAllocation:(id)allocation
{
	if (allocation == nil)
		return;
	pthread_mutex_lock(&_lock);
	[_allocations removeObject:allocation];
	pthread_mutex_unlock(&_lock);
}

- (void)removeAllocations:(const id *)allocations count:(NSUInteger)count
{
	if (allocations == NULL || count == 0)
		return;
	pthread_mutex_lock(&_lock);
	for (NSUInteger i = 0; i < count; i++) {
		if (allocations[i] != nil)
			[_allocations removeObject:allocations[i]];
	}
	pthread_mutex_unlock(&_lock);
}

- (void)removeAllAllocations
{
	pthread_mutex_lock(&_lock);
	[_allocations removeAllObjects];
	pthread_mutex_unlock(&_lock);
}

- (BOOL)containsAllocation:(id)anAllocation
{
	if (anAllocation == nil)
		return NO;
	pthread_mutex_lock(&_lock);
	BOOL present = [_allocations containsObject:anAllocation];
	pthread_mutex_unlock(&_lock);
	return present;
}

- (NSArray *)allAllocations
{
	pthread_mutex_lock(&_lock);
	NSArray *copy = [[_allocations array] copy];
	pthread_mutex_unlock(&_lock);
	return [copy autorelease];
}

- (NSUInteger)allocationCount
{
	pthread_mutex_lock(&_lock);
	NSUInteger n = [_allocations count];
	pthread_mutex_unlock(&_lock);
	return n;
}

- (void)commit
{
	pthread_mutex_lock(&_lock);
	uint64_t total = 0;
	for (id alloc in _allocations) {
		if ([alloc respondsToSelector:@selector(allocatedSize)])
			total += (uint64_t)[alloc allocatedSize];
	}
	_committedAllocatedSize = total;
	pthread_mutex_unlock(&_lock);
}

@end

@implementation AnnibaleMetalDevice (Residency)
- (id)newResidencySetWithDescriptor:(id)descriptor error:(NSError **)error
{
	AnnibaleMetalResidencySet *set =
	    [[AnnibaleMetalResidencySet alloc] initWithAnnibaleDevice:self descriptor:descriptor];
	if (set == nil && error != NULL) {
		*error = [NSError errorWithDomain:@"AnnibaleMetal" code:1
					 userInfo:@{ NSLocalizedDescriptionKey: @"out of memory creating residency set" }];
	}
	return set;
}
@end

@implementation AnnibaleMetalCommandQueue (Residency)
- (void)addResidencySet:(id)residencySet { (void)residencySet; }
- (void)addResidencySets:(const id *)residencySets count:(NSUInteger)count
{
	(void)residencySets; (void)count;
}
- (void)removeResidencySet:(id)residencySet { (void)residencySet; }
- (void)removeResidencySets:(const id *)residencySets count:(NSUInteger)count
{
	(void)residencySets; (void)count;
}
@end

@implementation AnnibaleMetalCommandBuffer (Residency)
- (void)useResidencySet:(id)residencySet { (void)residencySet; }
- (void)useResidencySets:(const id *)residencySets count:(NSUInteger)count
{
	(void)residencySets; (void)count;
}
@end
