//===-- combined.h ----------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef SCUDO_COMBINED_H_
#define SCUDO_COMBINED_H_

#include "chunk.h"
#include "common.h"
#include "flags.h"
#include "flags_parser.h"
#include "interface.h"
#include "local_cache.h"
#include "quarantine.h"
#include "report.h"
#include "secondary.h"
#include "tsd.h"

namespace scudo {

template <class Params> class Allocator {
public:
  typedef typename Params::Primary PrimaryT;
  typedef SizeClassAllocatorLocalCache<PrimaryT> CacheT;
  typedef Allocator<Params> ThisT;
  typedef typename Params::template TSDRegistryT<ThisT> TSDRegistryT;

  struct QuarantineCallback {
    explicit QuarantineCallback(ThisT &Instance, CacheT &LocalCache)
        : Allocator(Instance), Cache(LocalCache) {}

    // Chunk recycling function, returns a quarantined chunk to the backend,
    // first making sure it hasn't been tampered with.
    void recycle(void *Ptr) {
      UnpackedHeader Header;
      Chunk::loadHeader(Allocator.Cookie, Ptr, &Header);
      if (UNLIKELY(Header.State != ChunkQuarantine))
        reportInvalidChunkState(AllocatorAction::Recycling, Ptr);

      UnpackedHeader NewHeader = Header;
      NewHeader.State = ChunkAvailable;
      Chunk::compareExchangeHeader(Allocator.Cookie, Ptr, &NewHeader, &Header);

      void *BlockBegin = Chunk::getBlockBegin(Ptr, &Header);
      const uptr ClassId = Header.ClassId;
      if (ClassId)
        Cache.deallocate(&Allocator.Primary, ClassId, BlockBegin);
      else
        Allocator.Secondary.deallocate(BlockBegin);
    }

    // We take a shortcut when allocating a quarantine batch by working with the
    // appropriate class ID instead of using Size. The compiler should optimize
    // the class ID computation and work with the associated cache directly.
    void *allocate(UNUSED uptr Size) {
      const uptr QuarantineClassId = SizeClassMap::getClassIdBySize(
          sizeof(QuarantineBatch) + Chunk::getHeaderSize());
      void *Ptr = Cache.allocate(&Allocator.Primary, QuarantineClassId);
      // Quarantine batch allocation failure is fatal.
      if (UNLIKELY(!Ptr))
        reportOutOfMemory(SizeClassMap::getSizeByClassId(QuarantineClassId));

      Ptr = reinterpret_cast<void *>(reinterpret_cast<uptr>(Ptr) +
                                     Chunk::getHeaderSize());
      UnpackedHeader Header = {};
      Header.ClassId = QuarantineClassId & ClassIdMask;
      Header.SizeOrUnusedBytes = sizeof(QuarantineBatch);
      Header.State = ChunkAllocated;
      Chunk::storeHeader(Allocator.Cookie, Ptr, &Header);

      return Ptr;
    }

    void deallocate(void *Ptr) {
      const uptr QuarantineClassId = SizeClassMap::getClassIdBySize(
          sizeof(QuarantineBatch) + Chunk::getHeaderSize());
      UnpackedHeader Header;
      Chunk::loadHeader(Allocator.Cookie, Ptr, &Header);

      if (UNLIKELY(Header.State != ChunkAllocated))
        reportInvalidChunkState(AllocatorAction::Deallocating, Ptr);
      DCHECK_EQ(Header.ClassId, QuarantineClassId);
      DCHECK_EQ(Header.Offset, 0);
      DCHECK_EQ(Header.SizeOrUnusedBytes, sizeof(QuarantineBatch));

      UnpackedHeader NewHeader = Header;
      NewHeader.State = ChunkAvailable;
      Chunk::compareExchangeHeader(Allocator.Cookie, Ptr, &NewHeader, &Header);
      Cache.deallocate(&Allocator.Primary, QuarantineClassId,
                       reinterpret_cast<void *>(reinterpret_cast<uptr>(Ptr) -
                                                Chunk::getHeaderSize()));
    }

  private:
    ThisT &Allocator;
    CacheT &Cache;
  };

  typedef GlobalQuarantine<QuarantineCallback, void> QuarantineT;
  typedef typename QuarantineT::CacheT QuarantineCacheT;

  void initLinkerInitialized() {
    performSanityChecks();

    // Check if hardware CRC32 is supported in the binary and by the platform,
    // if so, opt for the CRC32 hardware version of the checksum.
    if (&computeHardwareCRC32 && hasHardwareCRC32())
      atomic_store_relaxed(&HashAlgorithm, HardwareCRC32);

    if (UNLIKELY(!getRandom(reinterpret_cast<void *>(&Cookie), sizeof(Cookie))))
      Cookie = static_cast<u32>(getMonotonicTime() ^
                                (reinterpret_cast<uptr>(this) >> 4));

    initFlags();
    reportUnrecognizedFlags();

    // Store some flags locally.
    Options.MayReturnNull = getFlags()->may_return_null;
    Options.ZeroContents = getFlags()->zero_contents;
    Options.DeallocTypeMismatch = getFlags()->dealloc_type_mismatch;
    Options.DeleteSizeMismatch = getFlags()->delete_size_mismatch;
    Options.QuarantineMaxChunkSize = getFlags()->quarantine_max_chunk_size;

    Stats.initLinkerInitialized();
    Primary.initLinkerInitialized(getFlags()->release_to_os_interval_ms);
    Secondary.initLinkerInitialized(&Stats);

    Quarantine.init(getFlags()->quarantine_size_kb << 10,
                    getFlags()->thread_local_quarantine_size_kb << 10);
  }

  void init() { memset(this, 0, sizeof(*this)); }

  TSDRegistryT *getTSDRegistry() { return &TSDRegistry; }

  void initCache(CacheT *Cache) { Cache->init(&Stats); }

  // Release the resources used by a TSD, which involves:
  // - draining the local quarantine cache to the global quarantine;
  // - releasing the cached pointers back to the Primary;
  // - unlinking the local stats from the global ones.
  void commitBack(TSD<ThisT> *TSD) {
    Quarantine.drain(&TSD->QuarantineCache,
                     QuarantineCallback(*this, TSD->Cache));
    TSD->Cache.destroy(&Primary, &Stats);
  }

  NOINLINE void *allocate(uptr Size, AllocType Type,
                          uptr Alignment = MinAlignment,
                          bool ZeroContents = false) {
    initThreadMaybe();

    if (UNLIKELY(Alignment > MaxAlignment)) {
      if (Options.MayReturnNull)
        return nullptr;
      reportAlignmentTooBig(Alignment, MaxAlignment);
    }
    if (UNLIKELY(Alignment < MinAlignment))
      Alignment = MinAlignment;

    // If the requested size happens to be 0 (more common than you might think),
    // allocate 1 byte on top of the header. Then add the extra bytes required
    // to fulfill the alignment requirements: we allocate enough to be sure that
    // there will be an address in the block that will be aligned.
    const uptr NeededSize =
        Chunk::getHeaderSize() + roundUpTo(Size ? Size : 1, MinAlignment) +
        ((Alignment > MinAlignment) ? (Alignment - Chunk::getHeaderSize()) : 0);

    // Takes care of extravagantly large sizes as well as integer overflows.
    if (UNLIKELY(Size >= MaxAllowedMallocSize ||
                 NeededSize >= MaxAllowedMallocSize)) {
      if (Options.MayReturnNull)
        return nullptr;
      reportAllocationSizeTooBig(Size, NeededSize, MaxAllowedMallocSize);
    }

    void *Block;
    uptr ClassId;
    uptr BlockEnd = 0;
    if (PrimaryT::canAllocate(NeededSize)) {
      ClassId = SizeClassMap::getClassIdBySize(NeededSize);
      bool UnlockRequired;
      auto *TSD = TSDRegistry.getTSDAndLock(&UnlockRequired);
      Block = TSD->Cache.allocate(&Primary, ClassId);
      if (UnlockRequired)
        TSD->unlock();
    } else {
      ClassId = 0;
      Block = Secondary.allocate(NeededSize, Alignment, &BlockEnd);
    }

    if (UNLIKELY(!Block)) {
      if (Options.MayReturnNull)
        return nullptr;
      reportOutOfMemory(Size);
    }

    // We only need to zero the contents for Primary backed allocations.
    if ((ZeroContents || Options.ZeroContents) && ClassId)
      memset(Block, 0, PrimaryT::getSizeByClassId(ClassId));

    UnpackedHeader Header = {};
    uptr UserPtr = reinterpret_cast<uptr>(Block) + Chunk::getHeaderSize();
    if (!isAligned(UserPtr, Alignment)) {
      const uptr AlignedUserPtr = roundUpTo(UserPtr, Alignment);
      const uptr Offset = AlignedUserPtr - UserPtr;
      Header.Offset = (Offset >> MinAlignmentLog) & OffsetMask;
      DCHECK_GT(Offset, 2 * sizeof(u32));
      // The BlockMarker has no security purpose, but is specifically meant for
      // the chunk iteration function that can be used in debugging situations.
      // It is the only situation where we have to locate the start of a chunk
      // based on its block address.
      reinterpret_cast<u32 *>(Block)[0] = BlockMarker;
      reinterpret_cast<u32 *>(Block)[1] = static_cast<u32>(Offset);
      UserPtr = AlignedUserPtr;
    }
    Header.State = ChunkAllocated;
    Header.AllocType = Type & AllocTypeMask;
    if (ClassId) {
      Header.ClassId = ClassId & ClassIdMask;
      Header.SizeOrUnusedBytes = Size & SizeOrUnusedBytesMask;
    } else {
      Header.SizeOrUnusedBytes =
          (BlockEnd - (UserPtr + Size)) & SizeOrUnusedBytesMask;
    }
    void *Ptr = reinterpret_cast<void *>(UserPtr);
    Chunk::storeHeader(Cookie, Ptr, &Header);

    if (&__scudo_allocate_hook)
      __scudo_allocate_hook(Ptr, Size);

    return Ptr;
  }

  void deallocate(void *Ptr, AllocType Type, uptr DeleteSize = 0,
                  UNUSED uptr Alignment = MinAlignment) {
    // For a deallocation, we only ensure minimal initialization, meaning thread
    // local data will be left uninitialized for now (when using ELF TLS). The
    // fallback cache will be used instead. This is a workaround for a situation
    // where the only heap operation performed in a thread would be a free past
    // the TLS destructors, ending up in initialized thread specific data never
    // being destroyed properly. Any other heap operation will do a full init.
    initThreadMaybe(/*MinimalInit=*/true);

    if (&__scudo_deallocate_hook)
      __scudo_deallocate_hook(Ptr);

    if (UNLIKELY(!Ptr))
      return;
    if (UNLIKELY(!isAligned(reinterpret_cast<uptr>(Ptr), MinAlignment)))
      reportMisalignedPointer(AllocatorAction::Deallocating, Ptr);

    UnpackedHeader Header;
    Chunk::loadHeader(Cookie, Ptr, &Header);

    if (UNLIKELY(Header.State != ChunkAllocated))
      reportInvalidChunkState(AllocatorAction::Deallocating, Ptr);
    if (Options.DeallocTypeMismatch) {
      if (Header.AllocType != Type) {
        // With the exception of memalign'd Chunks, that can be still be free'd.
        if (UNLIKELY(Header.AllocType != FromMemalign || Type != FromMalloc))
          reportDeallocTypeMismatch(AllocatorAction::Deallocating, Ptr,
                                    Header.AllocType, Type);
      }
    }

    const uptr Size = getSize(Ptr, &Header);
    if (DeleteSize && Options.DeleteSizeMismatch) {
      if (UNLIKELY(DeleteSize != Size))
        reportDeleteSizeMismatch(Ptr, DeleteSize, Size);
    }

    quarantineOrDeallocateChunk(Ptr, &Header, Size);
  }

  void *reallocate(void *OldPtr, uptr NewSize, uptr Alignment = MinAlignment) {
    initThreadMaybe();

    // The following cases are handled by the C wrappers.
    DCHECK_NE(OldPtr, nullptr);
    DCHECK_NE(NewSize, 0);

    if (UNLIKELY(!isAligned(reinterpret_cast<uptr>(OldPtr), MinAlignment)))
      reportMisalignedPointer(AllocatorAction::Reallocating, OldPtr);

    UnpackedHeader OldHeader;
    Chunk::loadHeader(Cookie, OldPtr, &OldHeader);

    if (UNLIKELY(OldHeader.State != ChunkAllocated))
      reportInvalidChunkState(AllocatorAction::Reallocating, OldPtr);

    // Pointer has to be allocated with a malloc-type function. Some
    // applications think that it is ok to realloc a memalign'ed pointer, which
    // will trigger this check.
    if (Options.DeallocTypeMismatch) {
      if (UNLIKELY(OldHeader.AllocType != FromMalloc))
        reportDeallocTypeMismatch(AllocatorAction::Reallocating, OldPtr,
                                  OldHeader.AllocType, FromMalloc);
    }

    const uptr OldSize = getSize(OldPtr, &OldHeader);
    // If the new size is identical to the old one, or lower but within an
    // acceptable range, we just keep the old chunk, and udpate its header.
    if (NewSize == OldSize)
      return OldPtr;
    if (NewSize < OldSize) {
      const uptr Delta = OldSize - NewSize;
      if (Delta < (SizeClassMap::MaxSize / 2)) {
        UnpackedHeader NewHeader = OldHeader;
        NewHeader.SizeOrUnusedBytes =
            (OldHeader.ClassId ? NewHeader.SizeOrUnusedBytes - Delta
                               : NewHeader.SizeOrUnusedBytes + Delta) &
            SizeOrUnusedBytesMask;
        Chunk::compareExchangeHeader(Cookie, OldPtr, &NewHeader, &OldHeader);
        return OldPtr;
      }
    }

    // Otherwise we allocate a new one, and deallocate the old one.
    void *NewPtr = allocate(NewSize, FromMalloc, Alignment);
    if (NewPtr) {
      memcpy(NewPtr, OldPtr, Min(NewSize, OldSize));
      quarantineOrDeallocateChunk(OldPtr, &OldHeader, OldSize);
    }
    return NewPtr;
  }

  // TODO(kostyak): while this locks the Primary & Secondary, it still allows
  //                pointers to be fetched from the TSD.
  void disable() {
    initThreadMaybe();
    Primary.disable();
    Secondary.disable();
  }

  void enable() {
    initThreadMaybe();
    Secondary.enable();
    Primary.enable();
  }

  void printStats() {
    disable();
    Primary.printStats();
    Secondary.printStats();
    Quarantine.printStats();
    enable();
  }

  void releaseToOS() { Primary.releaseToOS(); }

  // Iterate over all chunks and call a callback for all busy chunks located
  // within the provided memory range. Said callback must not use the heap or a
  // deadlock will ensue. This fits Android's malloc_iterate() needs.
  void iterateOverChunks(uptr Base, uptr Size, iterate_callback Callback,
                         void *Arg) {
    initThreadMaybe();
    const uptr From = Base;
    const uptr To = Base + Size;
    auto Lambda = [this, From, To, Callback, Arg](uptr Block) {
      if (Block < From || Block > To)
        return;
      uptr ChunkSize;
      const uptr ChunkBase = getChunkFromBlock(Block, &ChunkSize);
      if (ChunkBase != InvalidChunk)
        Callback(ChunkBase, ChunkSize, Arg);
    };
    Primary.iterateOverBlocks(Lambda);
    Secondary.iterateOverBlocks(Lambda);
  }

  bool canReturnNull() {
    initThreadMaybe();
    return Options.MayReturnNull;
  }

  // TODO(kostyak): implement this as a "backend" to mallopt.
  bool setOption(UNUSED uptr Option, UNUSED uptr Value) { return false; }

  // Return the usable size for a given chunk. Technically we lie, as we just
  // report the actual size of a chunk. This is done to counteract code actively
  // writing past the end of a chunk (like sqlite3) when the usable size allows
  // for it, which then forces realloc to copy the usable size of a chunk as
  // opposed to its actual size.
  uptr getUsableSize(const void *Ptr) {
    initThreadMaybe();
    if (UNLIKELY(!Ptr))
      return 0;
    UnpackedHeader Header;
    Chunk::loadHeader(Cookie, Ptr, &Header);
    // Getting the usable size of a chunk only makes sense if it's allocated.
    if (UNLIKELY(Header.State != ChunkAllocated))
      reportInvalidChunkState(AllocatorAction::Sizing, const_cast<void *>(Ptr));
    return getSize(Ptr, &Header);
  }

private:
  typedef LargeMmapAllocator SecondaryT;
  typedef typename PrimaryT::SizeClassMap SizeClassMap;

  static const uptr MinAlignmentLog = SCUDO_MIN_ALIGNMENT_LOG;
  static const uptr MaxAlignmentLog = 24; // 16 MB seems reasonable.
  static const uptr MinAlignment = 1U << MinAlignmentLog;
  static const uptr MaxAlignment = 1U << MaxAlignmentLog;
  static const uptr MaxAllowedMallocSize =
      FIRST_32_SECOND_64(1UL << 31, 1ULL << 40);

  // Constants used by the chunk iteration mechanism.
  static const u32 BlockMarker = 0x44554353U;
  static const uptr InvalidChunk = ~static_cast<uptr>(0);

  GlobalStats Stats;
  TSDRegistryT TSDRegistry;
  PrimaryT Primary;
  SecondaryT Secondary;
  QuarantineT Quarantine;

  u32 Cookie;

  struct {
    u8 MayReturnNull : 1;       // may_return_null
    u8 ZeroContents : 1;        // zero_contents
    u8 DeallocTypeMismatch : 1; // dealloc_type_mismatch
    u8 DeleteSizeMismatch : 1;  // delete_size_mismatch
    u32 QuarantineMaxChunkSize; // quarantine_max_chunk_size
  } Options;

  // The following might get optimized out by the compiler.
  NOINLINE void performSanityChecks() {
    // Verify that the header offset field can hold the maximum offset. In the
    // case of the Secondary allocator, it takes care of alignment and the
    // offset will always be small. In the case of the Primary, the worst case
    // scenario happens in the last size class, when the backend allocation
    // would already be aligned on the requested alignment, which would happen
    // to be the maximum alignment that would fit in that size class. As a
    // result, the maximum offset will be at most the maximum alignment for the
    // last size class minus the header size, in multiples of MinAlignment.
    UnpackedHeader Header = {};
    const uptr MaxPrimaryAlignment = 1 << getMostSignificantSetBitIndex(
                                         SizeClassMap::MaxSize - MinAlignment);
    const uptr MaxOffset =
        (MaxPrimaryAlignment - Chunk::getHeaderSize()) >> MinAlignmentLog;
    Header.Offset = MaxOffset & OffsetMask;
    if (UNLIKELY(Header.Offset != MaxOffset))
      reportSanityCheckError("offset");

    // Verify that we can fit the maximum size or amount of unused bytes in the
    // header. Given that the Secondary fits the allocation to a page, the worst
    // case scenario happens in the Primary. It will depend on the second to
    // last and last class sizes, as well as the dynamic base for the Primary.
    // The following is an over-approximation that works for our needs.
    const uptr MaxSizeOrUnusedBytes = SizeClassMap::MaxSize - 1;
    Header.SizeOrUnusedBytes = MaxSizeOrUnusedBytes & SizeOrUnusedBytesMask;
    if (UNLIKELY(Header.SizeOrUnusedBytes != MaxSizeOrUnusedBytes))
      reportSanityCheckError("size (or unused bytes)");

    const uptr LargestClassId = SizeClassMap::LargestClassId;
    Header.ClassId = LargestClassId;
    if (UNLIKELY(Header.ClassId != LargestClassId))
      reportSanityCheckError("class ID");
  }

  // Return the size of a chunk as requested during its allocation.
  INLINE uptr getSize(const void *Ptr, UnpackedHeader *Header) {
    const uptr SizeOrUnusedBytes = Header->SizeOrUnusedBytes;
    if (Header->ClassId)
      return SizeOrUnusedBytes;
    return SecondaryT::getBlockSize(Chunk::getBlockBegin(Ptr, Header)) -
           Chunk::getHeaderSize() - SizeOrUnusedBytes;
  }

  ALWAYS_INLINE void initThreadMaybe(bool MinimalInit = false) {
    TSDRegistry.initThreadMaybe(this, MinimalInit);
  }

  void quarantineOrDeallocateChunk(void *Ptr, UnpackedHeader *Header,
                                   uptr Size) {
    UnpackedHeader NewHeader = *Header;
    // If the quarantine is disabled, the actual size of a chunk is 0 or larger
    // than the maximum allowed, we return a chunk directly to the backend.
    const bool BypassQuarantine = !Quarantine.getCacheSize() || !Size ||
                                  (Size > Options.QuarantineMaxChunkSize);
    if (BypassQuarantine) {
      NewHeader.State = ChunkAvailable;
      Chunk::compareExchangeHeader(Cookie, Ptr, &NewHeader, Header);
      void *BlockBegin = Chunk::getBlockBegin(Ptr, Header);
      const uptr ClassId = NewHeader.ClassId;
      if (ClassId) {
        bool UnlockRequired;
        auto *TSD = TSDRegistry.getTSDAndLock(&UnlockRequired);
        TSD->Cache.deallocate(&Primary, ClassId, BlockBegin);
        if (UnlockRequired)
          TSD->unlock();
      } else {
        Secondary.deallocate(BlockBegin);
      }
    } else {
      NewHeader.State = ChunkQuarantine;
      Chunk::compareExchangeHeader(Cookie, Ptr, &NewHeader, Header);
      bool UnlockRequired;
      auto *TSD = TSDRegistry.getTSDAndLock(&UnlockRequired);
      Quarantine.put(&TSD->QuarantineCache,
                     QuarantineCallback(*this, TSD->Cache), Ptr, Size);
      if (UnlockRequired)
        TSD->unlock();
    }
  }

  // In its current use, this function only cares about valid busy chunks. This
  // might change in the future.
  uptr getChunkFromBlock(uptr Block, uptr *Size) {
    u32 Offset = 0;
    if (reinterpret_cast<u32 *>(Block)[0] == BlockMarker)
      Offset = reinterpret_cast<u32 *>(Block)[1];
    const uptr P = Block + Offset + Chunk::getHeaderSize();
    const void *Ptr = reinterpret_cast<const void *>(P);
    UnpackedHeader Header;
    if (!Chunk::isValid(Cookie, Ptr, &Header) || Header.State != ChunkAllocated)
      return InvalidChunk;
    if (Size)
      *Size = getSize(Ptr, &Header);
    return P;
  }
};

} // namespace scudo

#endif // SCUDO_COMBINED_H_