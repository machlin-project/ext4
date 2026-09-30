/* SPDX-License-Identifier: BSD-3-Clause */
#import "Ext4VolumeInternal.h"
#import "Ext4Support.h"

@implementation Ext4LegacyVolume

- (void)readFromFile:(FSItem *)item
	      offset:(off_t)offset
	      length:(size_t)length
	  intoBuffer:(FSMutableFileDataBuffer *)buffer
	replyHandler:(void (^)(size_t, NSError *))reply
{
	[self readFile:item offset:offset length:length intoBuffer:buffer replyHandler:reply];
}

- (void)writeContents:(NSData *)contents
	       toFile:(FSItem *)item
	     atOffset:(off_t)offset
	 replyHandler:(void (^)(size_t, NSError *))reply
{
	[self writeFile:item contents:contents offset:offset replyHandler:reply];
}

@end

#if defined(__MAC_27_0) && __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_27_0
@implementation Ext4ModernVolume

- (void)readFromFile:(FSItem *)item
	      offset:(off_t)offset
	      length:(size_t)length
	  intoBuffer:(FSMutableFileDataBuffer *)buffer
	replyHandler:(void (^)(FSReadFileResult *, NSError *))reply
{
	[self readFile:item
		  offset:offset
		  length:length
	      intoBuffer:buffer
	    replyHandler:^(size_t completed, NSError *error) {
	      FSReadFileResult *result = nil;

	      /* The engine calls back under the volume monitor. Publish the same
	       * inode snapshot that supplied these bytes, without a concurrent edit. */
	      if (error == nil) {
		      result = [[FSReadFileResult alloc]
			  initWithBytesRead:completed
			     itemAttributes:[self attributesForInode:&((Ext4Item *)item)->inode]];
		      if (result == nil) {
			      error = ext4_error(EXT4_IO);
		      }
	      }
	      reply(result, error);
	    }];
}

- (void)writeContents:(NSData *)contents
	       toFile:(FSItem *)item
	     atOffset:(off_t)offset
	 replyHandler:(void (^)(FSWriteFileResult *, NSError *))reply
{
	[self writeFile:item
		contents:contents
		  offset:offset
	    replyHandler:^(size_t completed, NSError *error) {
	      FSWriteFileResult *result = nil;
	      FSFreeSpace *space;
	      FSStatFSResult *statistics;

	      /* Handler-style FSKit ignores a result when error is non-nil. A
	       * committed prefix can instead be a short success if its current
	       * metadata is still readable. An aborted owner fails the refresh. */
	      if (completed != 0) {
		      error = nil;
	      }
	      /* In particular, size, allocation and privilege removal must reach
	       * FSKit's metadata cache in the same response as the write. Never
	       * publish the pre-write inode when a failed device cannot refresh it. */
	      if (error == nil) {
		      error = ext4_error([self validateItem:(Ext4Item *)item]);
	      }
	      if (error == nil) {
		      statistics = self.volumeStatistics;
		      space = [FSFreeSpace new];
		      [space populateWithBytes:statistics.availableBlocks * statistics.blockSize];
		      result = [[FSWriteFileResult alloc]
			  initWithBytesWritten:completed
				itemAttributes:[self attributesForInode:&((Ext4Item *)item)->inode]
				     freeSpace:space];
		      if (result == nil) {
			      error = ext4_error(EXT4_IO);
		      }
	      }
	      reply(result, error);
	    }];
}

@end
#endif

Ext4Volume *
ext4_volume_create(FSBlockDeviceResource *resource, struct ext4_fs *filesystem, id resourceOwner,
    struct ext4_native_crypto *crypto, BOOL writable)
{
	Class volumeClass = Ext4LegacyVolume.class;

#if defined(__MAC_27_0) && __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_27_0
	if (@available(macOS 27.0, *)) {
		volumeClass = Ext4ModernVolume.class;
	}
#endif
	return [[volumeClass alloc] initWithResource:resource
					  filesystem:filesystem
				       resourceOwner:resourceOwner
					      crypto:crypto
					    writable:writable];
}
