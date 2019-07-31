// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <unittest/unittest.h>
#include <fbl/intrusive_single_list.h>
#include <fbl/intrusive_hash_table.h>
#include <fbl/tests/intrusive_containers/associative_container_test_environment.h>
#include <fbl/tests/intrusive_containers/intrusive_hash_table_checker.h>
#include <fbl/tests/intrusive_containers/test_thunks.h>

namespace fbl {
namespace tests {
namespace intrusive_containers {

using OtherKeyType  = uint16_t;
using OtherHashType = uint32_t;
static constexpr OtherHashType kOtherNumBuckets = 23;

template <typename PtrType>
struct OtherHashTraits {
    using ObjType = typename ::fbl::internal::ContainerPtrTraits<PtrType>::ValueType;
    using BucketStateType = SinglyLinkedListNodeState<PtrType>;

    // Linked List Traits
    static BucketStateType& node_state(ObjType& obj) {
        return obj.other_container_state_.bucket_state_;
    }

    // Keyed Object Traits
    static OtherKeyType GetKey(const ObjType& obj) {
        return obj.other_container_state_.key_;
    }

    static bool LessThan(const OtherKeyType& key1, const OtherKeyType& key2) {
        return key1 <  key2;
    }

    static bool EqualTo(const OtherKeyType& key1, const OtherKeyType& key2) {
        return key1 == key2;
    }

    // Hash Traits
    static OtherHashType GetHash(const OtherKeyType& key) {
        return static_cast<OtherHashType>((key * 0xaee58187) % kOtherNumBuckets);
    }

    // Set key is a trait which is only used by the tests, not by the containers
    // themselves.
    static void SetKey(ObjType& obj, OtherKeyType key) {
        obj.other_container_state_.key_ = key;
    }
};

template <typename PtrType>
struct OtherHashState {
private:
    friend struct OtherHashTraits<PtrType>;
    OtherKeyType key_;
    typename OtherHashTraits<PtrType>::BucketStateType bucket_state_;
};

template <typename PtrType>
class HTSLLTraits {
public:
    using ObjType = typename ::fbl::internal::ContainerPtrTraits<PtrType>::ValueType;

    using ContainerType           = HashTable<size_t, PtrType>;
    using ContainableBaseClass    = SinglyLinkedListable<PtrType>;
    using ContainerStateType      = SinglyLinkedListNodeState<PtrType>;
    using KeyType                 = typename ContainerType::KeyType;
    using HashType                = typename ContainerType::HashType;

    using OtherContainerTraits    = OtherHashTraits<PtrType>;
    using OtherContainerStateType = OtherHashState<PtrType>;
    using OtherBucketType         = SinglyLinkedList<PtrType, OtherContainerTraits>;
    using OtherContainerType      = HashTable<OtherKeyType,
                                              PtrType,
                                              OtherBucketType,
                                              OtherHashType,
                                              kOtherNumBuckets,
                                              OtherContainerTraits,
                                              OtherContainerTraits>;

    using TestObjBaseType  = HashedTestObjBase<typename ContainerType::KeyType,
                                               typename ContainerType::HashType,
                                               ContainerType::kNumBuckets>;

    struct Tag1 {};
    struct Tag2 {};
    struct Tag3 {};

    using TaggedContainableBaseClasses =
        fbl::ContainableBaseClasses<SinglyLinkedListable<PtrType, Tag1>,
                                    SinglyLinkedListable<PtrType, Tag2>,
                                    SinglyLinkedListable<PtrType, Tag3>>;

    using TaggedType1 = TaggedHashTable<size_t, PtrType, Tag1>;
    using TaggedType2 = TaggedHashTable<size_t, PtrType, Tag2>;
    using TaggedType3 = TaggedHashTable<size_t, PtrType, Tag3>;
};

DEFINE_TEST_OBJECTS(HTSLL);
using UMTE    = DEFINE_TEST_THUNK(Associative, HTSLL, Unmanaged);
using UPTE    = DEFINE_TEST_THUNK(Associative, HTSLL, UniquePtr);
using SUPDDTE = DEFINE_TEST_THUNK(Associative, HTSLL, StdUniquePtrDefaultDeleter);
using SUPCDTE = DEFINE_TEST_THUNK(Associative, HTSLL, StdUniquePtrCustomDeleter);
using RPTE    = DEFINE_TEST_THUNK(Associative, HTSLL, RefPtr);

BEGIN_TEST_CASE(hashtable_sll_tests)
//////////////////////////////////////////
// General container specific tests.
//////////////////////////////////////////
RUN_NAMED_TEST("Clear (unmanaged)",                        UMTE::ClearTest)
RUN_NAMED_TEST("Clear (unique)",                           UPTE::ClearTest)
RUN_NAMED_TEST("Clear (std::uptr)",                        SUPDDTE::ClearTest)
RUN_NAMED_TEST("Clear (std::uptr<Del>)",                   SUPCDTE::ClearTest)
RUN_NAMED_TEST("Clear (RefPtr)",                           RPTE::ClearTest)

RUN_NAMED_TEST("ClearUnsafe (unmanaged)",                  UMTE::ClearUnsafeTest)
#if TEST_WILL_NOT_COMPILE || 0
RUN_NAMED_TEST("ClearUnsafe (unique)",                     UPTE::ClearUnsafeTest)
RUN_NAMED_TEST("ClearUnsafe (std::uptr)",                  SUPDDTE::ClearUnsafeTest)
RUN_NAMED_TEST("ClearUnsafe (std::uptr<Del>)",             SUPCDTE::ClearUnsafeTest)
RUN_NAMED_TEST("ClearUnsafe (RefPtr)",                     RPTE::ClearUnsafeTest)
#endif

RUN_NAMED_TEST("IsEmpty (unmanaged)",                      UMTE::IsEmptyTest)
RUN_NAMED_TEST("IsEmpty (unique)",                         UPTE::IsEmptyTest)
RUN_NAMED_TEST("IsEmpty (std::uptr)",                      SUPDDTE::IsEmptyTest)
RUN_NAMED_TEST("IsEmpty (std::uptr<Del>)",                 SUPCDTE::IsEmptyTest)
RUN_NAMED_TEST("IsEmpty (RefPtr)",                         RPTE::IsEmptyTest)

RUN_NAMED_TEST("Iterate (unmanaged)",                      UMTE::IterateTest)
RUN_NAMED_TEST("Iterate (unique)",                         UPTE::IterateTest)
RUN_NAMED_TEST("Iterate (std::uptr)",                      SUPDDTE::IterateTest)
RUN_NAMED_TEST("Iterate (std::uptr<Del>)",                 SUPCDTE::IterateTest)
RUN_NAMED_TEST("Iterate (RefPtr)",                         RPTE::IterateTest)

// Hashtables with singly linked list bucket can perform direct
// iterator/reference erase operations, but the operations will be O(n)
RUN_NAMED_TEST("IterErase (unmanaged)",                    UMTE::IterEraseTest)
RUN_NAMED_TEST("IterErase (unique)",                       UPTE::IterEraseTest)
RUN_NAMED_TEST("IterErase (std::uptr)",                    SUPDDTE::IterEraseTest)
RUN_NAMED_TEST("IterErase (std::uptr<Del>)",               SUPCDTE::IterEraseTest)
RUN_NAMED_TEST("IterErase (RefPtr)",                       RPTE::IterEraseTest)

RUN_NAMED_TEST("DirectErase (unmanaged)",                  UMTE::DirectEraseTest)
#if TEST_WILL_NOT_COMPILE || 0
RUN_NAMED_TEST("DirectErase (unique)",                     UPTE::DirectEraseTest)
RUN_NAMED_TEST("DirectErase (std::uptr)",                  SUPDDTE::DirectEraseTest)
RUN_NAMED_TEST("DirectErase (std::uptr<Del>)",             SUPCDTE::DirectEraseTest)
#endif
RUN_NAMED_TEST("DirectErase (RefPtr)",                     RPTE::DirectEraseTest)

RUN_NAMED_TEST("MakeIterator (unmanaged)",                 UMTE::MakeIteratorTest)
#if TEST_WILL_NOT_COMPILE || 0
RUN_NAMED_TEST("MakeIterator (unique)",                    UPTE::MakeIteratorTest)
RUN_NAMED_TEST("MakeIterator (std::uptr)",                 SUPDDTE::MakeIteratorTest)
RUN_NAMED_TEST("MakeIterator (std::uptr<Del>)",            SUPCDTE::MakeIteratorTest)
#endif
RUN_NAMED_TEST("MakeIterator (RefPtr)",                    RPTE::MakeIteratorTest)

// HashTables with SinglyLinkedList buckets cannot iterate backwards (because
// their buckets cannot iterate backwards)
#if TEST_WILL_NOT_COMPILE || 0
RUN_NAMED_TEST("ReverseIterErase (unmanaged)",             UMTE::ReverseIterEraseTest)
RUN_NAMED_TEST("ReverseIterErase (unique)",                UPTE::ReverseIterEraseTest)
RUN_NAMED_TEST("ReverseIterErase (std::uptr)",             SUPDDTE::ReverseIterEraseTest)
RUN_NAMED_TEST("ReverseIterErase (std::uptr<Del>)",        SUPCDTE::ReverseIterEraseTest)
RUN_NAMED_TEST("ReverseIterErase (RefPtr)",                RPTE::ReverseIterEraseTest)

RUN_NAMED_TEST("ReverseIterate (unmanaged)",               UMTE::ReverseIterateTest)
RUN_NAMED_TEST("ReverseIterate (unique)",                  UPTE::ReverseIterateTest)
RUN_NAMED_TEST("ReverseIterate (std::uptr)",               SUPDDTE::ReverseIterateTest)
RUN_NAMED_TEST("ReverseIterate (std::uptr<Del>)",          SUPCDTE::ReverseIterateTest)
RUN_NAMED_TEST("ReverseIterate (RefPtr)",                  RPTE::ReverseIterateTest)
#endif

// Hash tables do not support swapping or Rvalue operations (Assignment or
// construction) as doing so would be an O(n) operation (With 'n' == to the
// number of buckets in the hashtable)
#if TEST_WILL_NOT_COMPILE || 0
RUN_NAMED_TEST("Swap (unmanaged)",                         UMTE::SwapTest)
RUN_NAMED_TEST("Swap (unique)",                            UPTE::SwapTest)
RUN_NAMED_TEST("Swap (std::uptr)",                         SUPDDTE::SwapTest)
RUN_NAMED_TEST("Swap (std::uptr<Del>)",                    SUPCDTE::SwapTest)
RUN_NAMED_TEST("Swap (RefPtr)",                            RPTE::SwapTest)

RUN_NAMED_TEST("Rvalue Ops (unmanaged)",                   UMTE::RvalueOpsTest)
RUN_NAMED_TEST("Rvalue Ops (unique)",                      UPTE::RvalueOpsTest)
RUN_NAMED_TEST("Rvalue Ops (std::uptr)",                   SUPDDTE::RvalueOpsTest)
RUN_NAMED_TEST("Rvalue Ops (std::uptr<Del>)",              SUPCDTE::RvalueOpsTest)
RUN_NAMED_TEST("Rvalue Ops (RefPtr)",                      RPTE::RvalueOpsTest)
#endif

RUN_NAMED_TEST("Scope (unique)",                           UPTE::ScopeTest)
RUN_NAMED_TEST("Scope (std::uptr)",                        SUPDDTE::ScopeTest)
RUN_NAMED_TEST("Scope (std::uptr<Del>)",                   SUPCDTE::ScopeTest)
RUN_NAMED_TEST("Scope (RefPtr)",                           RPTE::ScopeTest)

RUN_NAMED_TEST("TwoContainer (unmanaged)",                 UMTE::TwoContainerTest)
#if TEST_WILL_NOT_COMPILE || 0
RUN_NAMED_TEST("TwoContainer (unique)",                    UPTE::TwoContainerTest)
RUN_NAMED_TEST("TwoContainer (std::uptr)",                 SUPDDTE::TwoContainerTest)
RUN_NAMED_TEST("TwoContainer (std::uptr<Del>)",            SUPCDTE::TwoContainerTest)
#endif
RUN_NAMED_TEST("TwoContainer (RefPtr)",                    RPTE::TwoContainerTest)

RUN_NAMED_TEST("ThreeContainerHelper (unmanaged)",         UMTE::ThreeContainerHelperTest)
#if TEST_WILL_NOT_COMPILE || 0
RUN_NAMED_TEST("ThreeContainerHelper (unique)",            UPTE::ThreeContainerHelperTest)
RUN_NAMED_TEST("ThreeContainerHelper (std::uptr)",         SUPDDTE::ThreeContainerHelperTest)
RUN_NAMED_TEST("ThreeContainerHelper (std::uptr<Del>)",    SUPCDTE::ThreeContainerHelperTest)
#endif
RUN_NAMED_TEST("ThreeContainerHelper (RefPtr)",            RPTE::ThreeContainerHelperTest)

RUN_NAMED_TEST("IterCopyPointer (unmanaged)",              UMTE::IterCopyPointerTest)
#if TEST_WILL_NOT_COMPILE || 0
RUN_NAMED_TEST("IterCopyPointer (unique)",                 UPTE::IterCopyPointerTest)
RUN_NAMED_TEST("IterCopyPointer (std::uptr)",              SUPDDTE::IterCopyPointerTest)
RUN_NAMED_TEST("IterCopyPointer (std::uptr<Del>)",         DDTE::IterCopyPointerTest)
#endif
RUN_NAMED_TEST("IterCopyPointer (RefPtr)",                 RPTE::IterCopyPointerTest)

RUN_NAMED_TEST("EraseIf (unmanaged)",                      UMTE::EraseIfTest)
RUN_NAMED_TEST("EraseIf (unique)",                         UPTE::EraseIfTest)
RUN_NAMED_TEST("EraseIf (std::uptr)",                      SUPCDTE::EraseIfTest)
RUN_NAMED_TEST("EraseIf (RefPtr)",                         RPTE::EraseIfTest)

RUN_NAMED_TEST("FindIf (unmanaged)",                       UMTE::FindIfTest)
RUN_NAMED_TEST("FindIf (unique)",                          UPTE::FindIfTest)
RUN_NAMED_TEST("FindIf (std::uptr)",                       SUPDDTE::FindIfTest)
RUN_NAMED_TEST("FindIf (std::uptr<Del>)",                  SUPCDTE::FindIfTest)
RUN_NAMED_TEST("FindIf (RefPtr)",                          RPTE::FindIfTest)

//////////////////////////////////////////
// Associative container specific tests.
//////////////////////////////////////////
RUN_NAMED_TEST("InsertByKey (unmanaged)",          UMTE::InsertByKeyTest)
RUN_NAMED_TEST("InsertByKey (unique)",             UPTE::InsertByKeyTest)
RUN_NAMED_TEST("InsertByKey (std::uptr)",          SUPDDTE::InsertByKeyTest)
RUN_NAMED_TEST("InsertByKey (std::uptr<Del>)",     SUPCDTE::InsertByKeyTest)
RUN_NAMED_TEST("InsertByKey (RefPtr)",             RPTE::InsertByKeyTest)

RUN_NAMED_TEST("FindByKey (unmanaged)",            UMTE::FindByKeyTest)
RUN_NAMED_TEST("FindByKey (unique)",               UPTE::FindByKeyTest)
RUN_NAMED_TEST("FindByKey (std::uptr)",            SUPDDTE::FindByKeyTest)
RUN_NAMED_TEST("FindByKey (std::uptr<Del>)",       SUPCDTE::FindByKeyTest)
RUN_NAMED_TEST("FindByKey (RefPtr)",               RPTE::FindByKeyTest)

RUN_NAMED_TEST("EraseByKey (unmanaged)",           UMTE::EraseByKeyTest)
RUN_NAMED_TEST("EraseByKey (unique)",              UPTE::EraseByKeyTest)
RUN_NAMED_TEST("EraseByKey (std::uptr)",           SUPDDTE::EraseByKeyTest)
RUN_NAMED_TEST("EraseByKey (std::uptr<Del>)",      SUPCDTE::EraseByKeyTest)
RUN_NAMED_TEST("EraseByKey (RefPtr)",              RPTE::EraseByKeyTest)

RUN_NAMED_TEST("InsertOrFind (unmanaged)",         UMTE::InsertOrFindTest)
RUN_NAMED_TEST("InsertOrFind (unique)",            UPTE::InsertOrFindTest)
RUN_NAMED_TEST("InsertOrFind (std::uptr)",         SUPDDTE::InsertOrFindTest)
RUN_NAMED_TEST("InsertOrFind (RefPtr)",            RPTE::InsertOrFindTest)

RUN_NAMED_TEST("InsertOrReplace (unmanaged)",      UMTE::InsertOrReplaceTest)
RUN_NAMED_TEST("InsertOrReplace (unique)",         UPTE::InsertOrReplaceTest)
RUN_NAMED_TEST("InsertOrReplace (std::uptr)",      SUPDDTE::InsertOrReplaceTest)
RUN_NAMED_TEST("InsertOrReplace (std::uptr<Del>)", SUPCDTE::InsertOrReplaceTest)
RUN_NAMED_TEST("InsertOrReplace (RefPtr)",         RPTE::InsertOrReplaceTest)
END_TEST_CASE(hashtable_sll_tests)

}  // namespace intrusive_containers
}  // namespace tests
}  // namespace fbl
