/* SPDX-License-Identifier: BSD-3-Clause */
#import "Ext4VolumeInternal.h"
#import "Ext4Support.h"
#include <errno.h>
#include <sys/stat.h>

#if defined(__MAC_27_0) && __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_27_0
static NSError *
ext4_handler_error(id result, NSError *error)
{
	return error != nil ? error : result == nil ? ext4_error(EXT4_IO) : nil;
}

@implementation Ext4ModernVolume

- (void)seekWithinItem:(FSItem *)file
	    fromOffset:(off_t)offset
		region:(FSSeekRegion)region
	       context:(FSContext *)context
	  replyHandler:(void (^)(FSSeekRegionResult *, NSError *))reply
{
	@synchronized(self) {
		Ext4Item *item = (Ext4Item *)file;
		uint64_t found = 0;
		FSSeekRegionResult *result = nil;
		NSError *error;
		enum ext4_result status = [self validateItem:item];

		(void)context;
		if (status == EXT4_OK &&
		    (offset < 0 || (region != FSSeekRegionHole && region != FSSeekRegionData))) {
			status = EXT4_INVALID_ARGUMENT;
		}
		if (status == EXT4_OK) {
			status = [self
			    seekItem:item
			      offset:(uint64_t)offset
			      region:region == FSSeekRegionHole ? EXT4_SEEK_HOLE : EXT4_SEEK_DATA
			      result:&found];
		}
		if (status == EXT4_OK && found > INT64_MAX) {
			status = EXT4_RANGE;
		}
		error = status == EXT4_NOT_FOUND
		    ? [NSError errorWithDomain:NSPOSIXErrorDomain code:ENXIO userInfo:nil]
		    : ext4_error(status);
		if (error == nil) {
			result = [[FSSeekRegionResult alloc] initWithReturnedOffset:(off_t)found];
		}
		reply(result, ext4_handler_error(result, error));
	}
}

/* Call only inside an engine completion, while its volume monitor is held.
 * In particular, a parent or overwritten inode can have changed even when it
 * wasn't the primary result of the portable operation. Never publish old data. */
- (FSItemAttributes *)refreshedAttributesForItem:(FSItem *)file error:(NSError **)error
{
	Ext4Item *item = (Ext4Item *)file;

	if (*error != nil) {
		return nil;
	}
	*error = ext4_error([self validateItem:item]);
	return *error == nil ? [self attributesForInode:&item->inode] : nil;
}

- (FSFreeSpace *)currentFreeSpace
{
	FSStatFSResult *statistics = self.volumeStatistics;
	FSFreeSpace *space = [FSFreeSpace new];

	[space populateWithBytes:statistics.availableBlocks * statistics.blockSize];
	return space;
}

- (void)activateVolumeWithOptions:(FSTaskOptions *)options
		     replyHandler:(void (^)(FSActivateResult *, NSError *))reply
{
	[self activateWithOptions:options
		     replyHandler:^(FSItem *root, NSError *error) {
		       FSActivateResult *result =
			   error == nil ? [[FSActivateResult alloc] initWithRootItem:root] : nil;

		       reply(result, ext4_handler_error(result, error));
		     }];
}

- (void)deactivateVolumeWithOptions:(FSDeactivateOptions)options
		       replyHandler:(void (^)(NSError *))reply
{
	[self deactivateWithOptions:options replyHandler:reply];
}

- (void)lookupResultNamed:(FSFileName *)name
	      inDirectory:(FSItem *)directory
	      resultClass:(Class)resultClass
	     replyHandler:(void (^)(FSLookupItemResult *, NSError *))reply
{
	[self
	    lookupItemNamed:name
		inDirectory:directory
	       replyHandler:^(FSItem *item, FSFileName *foundName, NSError *error) {
		 FSLookupItemResult *result = nil;

		 if (error == nil) {
			 result = [[resultClass alloc]
			     initWithFoundItem:item
				      itemName:foundName
				itemAttributes:[self
						   attributesForInode:&((Ext4Item *)item)->inode]];
		 }
		 reply(result, ext4_handler_error(result, error));
	       }];
}

- (void)lookupItemNamed:(FSFileName *)name
	    inDirectory:(FSItem *)directory
		context:(FSContext *)context
	   replyHandler:(void (^)(FSLookupItemResult *, NSError *))reply
{
	(void)context;
	[self lookupResultNamed:name
		    inDirectory:directory
		    resultClass:FSLookupItemResult.class
		   replyHandler:reply];
}

- (void)lookupItemNamed:(FSFileName *)name
	    inDirectory:(FSItem *)directory
		 packer:(FSExtentPacker *)packer
		context:(FSContext *)context
	   replyHandler:(void (^)(FSLookupItemKOIOResult *, NSError *))reply
{
	(void)context;
	(void)packer;
	[self lookupResultNamed:name
		    inDirectory:directory
		    resultClass:FSLookupItemKOIOResult.class
		   replyHandler:^(FSLookupItemResult *result, NSError *error) {
		     reply((FSLookupItemKOIOResult *)result, error);
		   }];
}

- (void)createResultNamed:(FSFileName *)name
		     type:(FSItemType)type
		   parent:(FSItem *)directory
	       attributes:(FSItemSetAttributesRequest *)attributes
		     link:(FSFileName *)link
	      resultClass:(Class)resultClass
		    reply:(void (^)(FSCreateItemResult *, NSError *))reply
{
	[self createNamed:name
		     type:type
		   parent:directory
	       attributes:attributes
		     link:link
		    reply:^(FSItem *item, FSFileName *createdName, NSError *error) {
		      FSCreateItemResult *result = nil;
		      FSItemAttributes *parentAttributes =
			  [self refreshedAttributesForItem:directory error:&error];

		      if (error == nil) {
			      result = [[resultClass alloc]
				      initWithNewItem:item
					  newItemName:createdName
				    newItemAttributes:[self attributesForInode:&((Ext4Item *)item)
										   ->inode]
				  directoryAttributes:parentAttributes
					    freeSpace:[self currentFreeSpace]];
		      }
		      reply(result, ext4_handler_error(result, error));
		    }];
}

- (void)createItemNamed:(FSFileName *)name
		   type:(FSItemType)type
	    inDirectory:(FSItem *)directory
	     attributes:(FSItemSetAttributesRequest *)attributes
		context:(FSContext *)context
	   replyHandler:(void (^)(FSCreateItemResult *, NSError *))reply
{
	(void)context;
	[self createResultNamed:name
			   type:type
			 parent:directory
		     attributes:attributes
			   link:nil
		    resultClass:FSCreateItemResult.class
			  reply:reply];
}

- (void)createFileNamed:(FSFileName *)name
	    inDirectory:(FSItem *)directory
	     attributes:(FSItemSetAttributesRequest *)attributes
		 packer:(FSExtentPacker *)packer
		context:(FSContext *)context
	   replyHandler:(void (^)(FSCreateFileKOIOResult *, NSError *))reply
{
	(void)context;
	(void)packer;
	[self createResultNamed:name
			   type:FSItemTypeFile
			 parent:directory
		     attributes:attributes
			   link:nil
		    resultClass:FSCreateFileKOIOResult.class
			  reply:^(FSCreateItemResult *result, NSError *error) {
			    reply((FSCreateFileKOIOResult *)result, error);
			  }];
}

- (void)createSymbolicLinkNamed:(FSFileName *)name
		    inDirectory:(FSItem *)directory
		     attributes:(FSItemSetAttributesRequest *)attributes
		   linkContents:(FSFileName *)contents
			context:(FSContext *)context
		   replyHandler:(void (^)(FSCreateSymlinkResult *, NSError *))reply
{
	(void)context;
	[self createResultNamed:name
			   type:FSItemTypeSymlink
			 parent:directory
		     attributes:attributes
			   link:contents
		    resultClass:FSCreateSymlinkResult.class
			  reply:^(FSCreateItemResult *result, NSError *error) {
			    reply((FSCreateSymlinkResult *)result, error);
			  }];
}

- (void)createLinkToItem:(FSItem *)item
		   named:(FSFileName *)name
	     inDirectory:(FSItem *)directory
		 context:(FSContext *)context
	    replyHandler:(void (^)(FSCreateLinkResult *, NSError *))reply
{
	(void)context;
	[self linkItem:item
		   named:name
	     inDirectory:directory
	    replyHandler:^(FSFileName *linkName, NSError *error) {
	      FSCreateLinkResult *result = nil;
	      FSItemAttributes *parentAttributes = [self refreshedAttributesForItem:directory
									      error:&error];

	      if (error == nil) {
		      result = [[FSCreateLinkResult alloc]
			     initWithLinkName:linkName
			       linkAttributes:[self attributesForInode:&((Ext4Item *)item)->inode]
			  directoryAttributes:parentAttributes
				    freeSpace:[self currentFreeSpace]];
	      }
	      reply(result, ext4_handler_error(result, error));
	    }];
}

- (void)renameItem:(FSItem *)item
       inDirectory:(FSItem *)sourceDirectory
	     named:(FSFileName *)sourceName
	 toNewName:(FSFileName *)destinationName
       inDirectory:(FSItem *)destinationDirectory
	  overItem:(FSItem *)overItem
	   context:(FSContext *)context
      replyHandler:(void (^)(FSRenameItemResult *, NSError *))reply
{
	(void)context;
	[self moveItem:item
	     inDirectory:sourceDirectory
		   named:sourceName
	       toNewName:destinationName
	     inDirectory:destinationDirectory
		overItem:overItem
	    replyHandler:^(FSFileName *newName, NSError *error) {
	      FSRenameItemResult *result = nil;
	      FSItemAttributes *source = [self refreshedAttributesForItem:sourceDirectory
								    error:&error];
	      FSItemAttributes *destination = destinationDirectory == sourceDirectory
		  ? source
		  : [self refreshedAttributesForItem:destinationDirectory error:&error];
	      FSItemAttributes *over =
		  overItem == nil ? nil : [self refreshedAttributesForItem:overItem error:&error];

	      if (error == nil) {
		      result = [[FSRenameItemResult alloc]
					 initWithNewName:newName
				   renamedItemAttributes:[self
							     attributesForInode:&((Ext4Item *)item)
										    ->inode]
			       sourceDirectoryAttributes:source
			  destinationDirectoryAttributes:destination
				      overItemAttributes:over
					       freeSpace:[self currentFreeSpace]];
	      }
	      reply(result, ext4_handler_error(result, error));
	    }];
}

- (void)removeItem:(FSItem *)item
	     named:(FSFileName *)name
     fromDirectory:(FSItem *)directory
	   context:(FSContext *)context
      replyHandler:(void (^)(FSRemoveItemResult *, NSError *))reply
{
	(void)context;
	[self deleteItem:item
		    named:name
	    fromDirectory:directory
	     replyHandler:^(NSError *error) {
	       FSRemoveItemResult *result = nil;
	       FSItemAttributes *parentAttributes = [self refreshedAttributesForItem:directory
									       error:&error];

	       if (error == nil) {
		       result = [[FSRemoveItemResult alloc]
			   initWithItemAttributes:[self
						      attributesForInode:&((Ext4Item *)item)->inode]
			      directoryAttributes:parentAttributes
					freeSpace:[self currentFreeSpace]];
	       }
	       reply(result, ext4_handler_error(result, error));
	     }];
}

- (void)getAttributes:(FSItemGetAttributesRequest *)request
	       ofItem:(FSItem *)item
	      context:(FSContext *)context
	 replyHandler:(void (^)(FSGetAttributesResult *, NSError *))reply
{
	(void)context;
	[self getAttributes:request
		     ofItem:item
	       replyHandler:^(FSItemAttributes *attributes, NSError *error) {
		 FSGetAttributesResult *result = error == nil
		     ? [[FSGetAttributesResult alloc] initWithAttributes:attributes]
		     : nil;

		 reply(result, ext4_handler_error(result, error));
	       }];
}

- (void)setAttributes:(FSItemSetAttributesRequest *)request
	       onItem:(FSItem *)item
	      context:(FSContext *)context
	 replyHandler:(void (^)(FSSetAttributesResult *, NSError *))reply
{
	(void)context;
	[self changeAttributes:request
			onItem:item
		  replyHandler:^(FSItemAttributes *attributes, NSError *error) {
		    FSSetAttributesResult *result = error == nil
			? [[FSSetAttributesResult alloc] initWithAttributes:attributes
								  freeSpace:[self currentFreeSpace]]
			: nil;

		    reply(result, ext4_handler_error(result, error));
		  }];
}

- (void)enumerateDirectory:(FSItem *)directory
	  startingAtCookie:(FSDirectoryCookie)cookie
		  verifier:(FSDirectoryVerifier)verifier
       providingAttributes:(FSItemGetAttributesRequest *)attributes
	       usingPacker:(FSDirectoryEntryPacker *)packer
		   context:(FSContext *)context
	      replyHandler:(void (^)(FSEnumerateDirectoryResult *, NSError *))reply
{
	(void)context;
	[self enumerateDirectory:directory
		startingAtCookie:cookie
			verifier:verifier
	     providingAttributes:attributes
		     usingPacker:packer
		    replyHandler:^(FSDirectoryVerifier current, NSError *error) {
		      FSEnumerateDirectoryResult *result = error == nil
			  ? [[FSEnumerateDirectoryResult alloc] initWithVerifier:current]
			  : nil;

		      reply(result, ext4_handler_error(result, error));
		    }];
}

- (void)readSymbolicLink:(FSItem *)item
		 context:(FSContext *)context
	    replyHandler:(void (^)(FSReadSymlinkResult *, NSError *))reply
{
	(void)context;
	[self
	    readSymbolicLink:item
		replyHandler:^(FSFileName *contents, NSError *error) {
		  FSReadSymlinkResult *result = error == nil
		      ? [[FSReadSymlinkResult alloc]
			     initWithContents:contents
			    symlinkAttributes:[self attributesForInode:&((Ext4Item *)item)->inode]]
		      : nil;

		  reply(result, ext4_handler_error(result, error));
		}];
}

- (void)openItem:(FSItem *)item
       withModes:(FSVolumeOpenModes)modes
	 context:(FSContext *)context
    replyHandler:(void (^)(NSError *))reply
{
	(void)context;
	[self openItem:item withModes:modes replyHandler:reply];
}

- (void)closeItem:(FSItem *)item
     keepingModes:(FSVolumeOpenModes)modes
	  context:(FSContext *)context
     replyHandler:(void (^)(NSError *))reply
{
	(void)context;
	[self closeItem:item keepingModes:modes replyHandler:reply];
}

- (void)getXattrNamed:(FSFileName *)name
	       ofItem:(FSItem *)item
	      context:(FSContext *)context
	 replyHandler:(void (^)(FSGetXattrResult *, NSError *))reply
{
	(void)context;
	[self getXattrNamed:name
		     ofItem:item
	       replyHandler:^(NSData *value, NSError *error) {
		 FSGetXattrResult *result =
		     error == nil ? [[FSGetXattrResult alloc] initWithXattrValue:value] : nil;

		 reply(result, ext4_handler_error(result, error));
	       }];
}

- (void)setXattrNamed:(FSFileName *)name
	       toData:(NSData *)value
	       onItem:(FSItem *)item
	       policy:(FSSetXattrPolicy)policy
	      context:(FSContext *)context
	 replyHandler:(void (^)(FSSetXattrResult *, NSError *))reply
{
	(void)context;
	[self setXattrNamed:name
		     toData:value
		     onItem:item
		     policy:policy
	       replyHandler:^(NSError *error) {
		 FSSetXattrResult *result = error == nil
		     ? [[FSSetXattrResult alloc] initWithFreeSpace:[self currentFreeSpace]]
		     : nil;

		 reply(result, ext4_handler_error(result, error));
	       }];
}

- (void)listXattrsOfItem:(FSItem *)item
		 context:(FSContext *)context
	    replyHandler:(void (^)(FSListXattrsResult *, NSError *))reply
{
	(void)context;
	[self listXattrsOfItem:item
		  replyHandler:^(NSArray<FSFileName *> *names, NSError *error) {
		    FSListXattrsResult *result =
			error == nil ? [[FSListXattrsResult alloc] initWithXattrNames:names] : nil;

		    reply(result, ext4_handler_error(result, error));
		  }];
}

- (void)deactivateItem:(FSItem *)item
	       context:(FSContext *)context
	  replyHandler:(void (^)(FSDeactivateItemResult *, NSError *))reply
{
	(void)context;
	[self deactivateItem:item
		replyHandler:^(NSError *error) {
		  FSDeactivateItemResult *result = error == nil
		      ? [[FSDeactivateItemResult alloc] initWithFreeSpace:[self currentFreeSpace]]
		      : nil;

		  reply(result, ext4_handler_error(result, error));
		}];
}

- (void)preallocateSpaceForItem:(FSItem *)item
		       atOffset:(off_t)offset
			 length:(size_t)length
			  flags:(FSPreallocateFlags)flags
			context:(FSContext *)context
		   replyHandler:(void (^)(FSPreallocateResult *, NSError *))reply
{
	(void)context;
	[self preallocateSpaceForItem:item
			     atOffset:offset
			       length:length
				flags:flags
			 replyHandler:^(size_t completed, NSError *error) {
			   FSPreallocateResult *result = nil;
			   FSItemAttributes *attributes = [self refreshedAttributesForItem:item
										     error:&error];

			   if (error == nil) {
				   result = [[FSPreallocateResult alloc]
				       initWithBytesAllocated:completed
					       itemAttributes:attributes
						    freeSpace:[self currentFreeSpace]];
			   }
			   reply(result, ext4_handler_error(result, error));
			 }];
}

- (void)setVolumeName:(FSFileName *)name
	      context:(FSContext *)context
	 replyHandler:(void (^)(FSVolumeRenameResult *, NSError *))reply
{
	(void)context;
	[self setVolumeName:name
	       replyHandler:^(FSFileName *newName, NSError *error) {
		 FSVolumeRenameResult *result =
		     error == nil ? [[FSVolumeRenameResult alloc] initWithNewName:newName] : nil;

		 reply(result, ext4_handler_error(result, error));
	       }];
}

- (void)blockmapFile:(FSItem *)file
	      offset:(off_t)offset
	      length:(size_t)length
	       flags:(FSBlockmapFlags)flags
	 operationID:(FSOperationID)operationID
	      packer:(FSExtentPacker *)packer
	replyHandler:(void (^)(FSBlockmapResult *, NSError *))reply
{
	[self mapFile:file
		  offset:offset
		  length:length
		   flags:flags
	     operationID:operationID
		  packer:packer
	    replyHandler:^(NSError *error) {
	      FSBlockmapResult *result = error == nil
		  ? [[FSBlockmapResult alloc] initWithFreeSpace:[self currentFreeSpace]]
		  : nil;

	      reply(result, ext4_handler_error(result, error));
	    }];
}

- (void)completeIOForFile:(FSItem *)file
		   offset:(off_t)offset
		   length:(size_t)length
		   status:(NSError *)status
		    flags:(FSCompleteIOFlags)flags
	      operationID:(FSOperationID)operationID
	     replyHandler:(void (^)(FSCompleteIOResult *, NSError *))reply
{
	@synchronized(self) {
		[self finishIOForFile:file
			       offset:offset
			       length:length
			       status:status
				flags:flags
			  operationID:operationID
			 replyHandler:^(NSError *error) {
			   FSItemAttributes *attributes = [self refreshedAttributesForItem:file
										     error:&error];
			   FSCompleteIOResult *result = error == nil
			       ? [[FSCompleteIOResult alloc] initWithAttributes:attributes]
			       : nil;

			   reply(result, ext4_handler_error(result, error));
			 }];
	}
}

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
	      FSItemAttributes *attributes;
#if DEBUG
	      uint32_t previousMode =
		  completed != 0 ? ((Ext4Item *)item)->inode.mode & ALLPERMS : 0;
	      NSError *writeError = error;
	      BOOL traceResult = error != nil || completed != contents.length;
#endif

	      /* Handler-style FSKit requires ENOSPC on allocation failure and ignores
	       * the result on error. Never turn a committed prefix into successful
	       * completion of a larger kernel I/O; FSKit does not retry its suffix. */
	      /* In particular, size, allocation and privilege removal must reach
	       * FSKit's metadata cache in the same response as the write. Never
	       * publish the pre-write inode when a failed device cannot refresh it. */
	      attributes = [self refreshedAttributesForItem:item error:&error];
	      if (error == nil) {
		      result =
			  [[FSWriteFileResult alloc] initWithBytesWritten:completed
							   itemAttributes:attributes
								freeSpace:[self currentFreeSpace]];
	      }
	      error = ext4_handler_error(result, error);
#if DEBUG
	      if (traceResult) {
		      NSLog(@"Machlin ext4 modern write: offset=%lld requested=%lu completed=%lu "
			    @"core-error=%@ result=%d reply-error=%@",
			  (long long)offset, (unsigned long)contents.length,
			  (unsigned long)completed, writeError, result != nil, error);
	      }
	      if ((previousMode & (S_ISUID | S_ISGID)) != 0) {
		      NSLog(@"Machlin ext4 write attributes: inode=%u previous-mode=%o "
			    @"returned-mode=%o wanted-mode=%d result=%d error=%@",
			  ((Ext4Item *)item)->inode.number, previousMode, attributes.mode,
			  [FSWriteFileResult.requestedAttributes
			      isAttributeWanted:FSItemAttributeMode],
			  result != nil, error);
	      }
#endif
	      reply(result, error);
	    }];
}

@end
#endif
