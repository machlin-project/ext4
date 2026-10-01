/* SPDX-License-Identifier: BSD-3-Clause */
#import "Ext4VolumeInternal.h"

@implementation Ext4Volume (ReadState)

- (void)startReadStateMaintenance
{
	__weak Ext4Volume *weakSelf = self;

	if (!_active || _memoryPressureSource != nil) {
		return;
	}
	_memoryPressureSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0,
	    DISPATCH_MEMORYPRESSURE_NORMAL | DISPATCH_MEMORYPRESSURE_WARN |
		DISPATCH_MEMORYPRESSURE_CRITICAL,
	    dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
	if (_memoryPressureSource == nil) {
		/* Failure to observe pressure conservatively disables optional caching. */
		_memoryPressureRaised = YES;
		return;
	}
	dispatch_source_set_event_handler(_memoryPressureSource, ^{
	  Ext4Volume *volume = weakSelf;

	  if (volume == nil) {
		  return;
	  }
	  @synchronized(volume) {
		  if (volume->_memoryPressureSource != nil) {
			  [volume updateMemoryPressure:dispatch_source_get_data(
							   volume->_memoryPressureSource)];
		  }
	  }
	});
	dispatch_resume(_memoryPressureSource);
}

- (void)stopReadStateMaintenance
{
	if (_memoryPressureSource != nil) {
		dispatch_source_cancel(_memoryPressureSource);
		_memoryPressureSource = nil;
	}
}

- (void)updateMemoryPressure:(dispatch_source_memorypressure_flags_t)flags
{
	@synchronized(self) {
		if (!_active) {
			return;
		}
		/* Dispatch may coalesce notifications. Elevated pressure takes precedence
		 * over normal; preserve the user's preference throughout the transition.
		 * Do not traverse old caches here: paging them in worsens pressure. */
		if ((flags & (DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL)) !=
		    0) {
			_memoryPressureRaised = YES;
		} else if ((flags & DISPATCH_MEMORYPRESSURE_NORMAL) != 0) {
			_memoryPressureRaised = NO;
		}
	}
}

/* The caller holds the volume monitor, including held read/map completion. */
- (BOOL)readStateRetentionActive
{
	return _retainReadState && !_memoryPressureRaised;
}

- (void)finishReadStateForItem:(Ext4Item *)item
{
	if (![self readStateRetentionActive]) {
		ext4_drop_read_cache(item->hold);
	}
}

- (void)dropReadState
{
	for (Ext4Item *item in _items.objectEnumerator) {
		if (item->hold != NULL) {
			ext4_drop_read_cache(item->hold);
		}
	}
}

@end
