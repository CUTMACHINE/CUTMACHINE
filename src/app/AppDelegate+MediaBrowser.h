#pragma once

// STRUCTURE-2026-11 ST4 -- docs/roadmap/STRUCTURE-2026-11.md. The media
// browser: the bins' outline, the media list and icon grid as data sources
// and delegates, their drags and drops, the selection, renaming, moving
// into bins, and opening a media in the Source monitor or as a timeline.
// MediaPanelModel.h decides what is shown; these methods relay. Moved out
// of main.mm unchanged.

@interface AppDelegate (MediaBrowser) <NSOutlineViewDataSource,
                                       NSOutlineViewDelegate,
                                       NSTableViewDataSource,
                                       NSTableViewDelegate,
                                       NSCollectionViewDataSource,
                                       NSCollectionViewDelegate,
                                       NSTextFieldDelegate>
- (void)refreshBinControlsSelecting:(NSString*)selectedBinId;
- (void)binSelectionChanged:(id)sender;
- (NSArray<NSString*>*)childBinIds:(NSString*)parentId;
- (NSString*)dropTargetBinForItem:(id)item;
- (BOOL)bin:(NSString*)binId canMoveInto:(NSString*)parentId;
- (void)rebuildMediaList;
- (void)mediaViewChanged:(id)sender;
- (NSString*)selectedMediaId;
- (NSArray<NSString*>*)selectedMediaIds;
- (void)openIconBin:(NSClickGestureRecognizer*)recognizer;
- (void)openSelectedMediaInSourceMonitor:(id)sender;
- (void)createTimelineFromSelectedMedia:(id)sender;
- (void)createTimelineNamed:(NSString*)trimmed
                  fromMedia:(const std::vector<Ulid>&)mediaIds;
- (void)openMediaIdentifierInSourceMonitor:(NSString*)identifier;
- (void)revealSelectedMediaInFinder:(id)sender;
- (NSInteger)numberOfRowsInTableView:(NSTableView*)tableView;
- (void)mediaSearchChanged:(id)sender;
- (void)createBinPressed:(id)sender;
- (void)deleteBinPressed:(id)sender;
- (void)renameBinPressed:(id)sender;
- (void)beginEditingBin:(NSString*)binId;
- (void)controlTextDidEndEditing:(NSNotification*)notification;
- (void)assignMediaToBinPressed:(id)sender;
- (void)moveMediaIdsToBin:(NSArray<NSString*>*)mediaIdentifiers;
- (void)moveMediaIds:(NSArray<NSString*>*)mediaIds toBin:(NSString*)bin;
- (void)contextFindClipInMediaPool:(id)sender;
- (NSInteger)outlineView:(NSOutlineView*)outlineView
    numberOfChildrenOfItem:(id)item;
- (id)outlineView:(NSOutlineView*)outlineView
            child:(NSInteger)index
           ofItem:(id)item;
- (BOOL)outlineView:(NSOutlineView*)outlineView isItemExpandable:(id)item;
- (id<NSPasteboardWriting>)outlineView:(NSOutlineView*)outlineView
               pasteboardWriterForItem:(id)item;
- (NSDragOperation)outlineView:(NSOutlineView*)outlineView
                  validateDrop:(id<NSDraggingInfo>)info
                  proposedItem:(id)item
            proposedChildIndex:(NSInteger)index;
- (BOOL)outlineView:(NSOutlineView*)outlineView
         acceptDrop:(id<NSDraggingInfo>)info
               item:(id)item
         childIndex:(NSInteger)index;
- (NSView*)outlineView:(NSOutlineView*)outlineView
    viewForTableColumn:(NSTableColumn*)tableColumn
                  item:(id)item;
- (NSTableRowView*)outlineView:(NSOutlineView*)outlineView
                rowViewForItem:(id)item;
- (void)outlineViewSelectionDidChange:(NSNotification*)notification;
- (NSInteger)collectionView:(NSCollectionView*)collectionView
     numberOfItemsInSection:(NSInteger)section;
- (NSCollectionViewItem*)collectionView:(NSCollectionView*)collectionView
    itemForRepresentedObjectAtIndexPath:(NSIndexPath*)indexPath;
- (id<NSPasteboardWriting>)collectionView:(NSCollectionView*)collectionView
       pasteboardWriterForItemAtIndexPath:(NSIndexPath*)indexPath;
- (BOOL)collectionView:(NSCollectionView*)collectionView
    canDragItemsAtIndexPaths:(NSSet<NSIndexPath*>*)indexPaths
                   withEvent:(NSEvent*)event;
- (NSDragOperation)collectionView:(NSCollectionView*)collectionView
                     validateDrop:(id<NSDraggingInfo>)draggingInfo
                proposedIndexPath:(NSIndexPath**)proposedDropIndexPath
                    dropOperation:
                        (NSCollectionViewDropOperation*)proposedDropOperation;
- (BOOL)collectionView:(NSCollectionView*)collectionView
            acceptDrop:(id<NSDraggingInfo>)draggingInfo
             indexPath:(NSIndexPath*)indexPath
         dropOperation:(NSCollectionViewDropOperation)dropOperation;
- (BOOL)tableView:(NSTableView*)tableView
    writeRowsWithIndexes:(NSIndexSet*)rowIndexes
            toPasteboard:(NSPasteboard*)pasteboard;
- (NSTableRowView*)tableView:(NSTableView*)tableView
               rowViewForRow:(NSInteger)row;
- (NSView*)tableView:(NSTableView*)tableView
    viewForTableColumn:(NSTableColumn*)tableColumn
                   row:(NSInteger)row;
- (void)tableViewSelectionDidChange:(NSNotification*)notification;
- (void)collectionView:(NSCollectionView*)collectionView
    didSelectItemsAtIndexPaths:(NSSet<NSIndexPath*>*)indexPaths;
@end
