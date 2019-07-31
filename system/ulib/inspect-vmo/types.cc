// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fbl/string_piece.h"
#include <lib/inspect-vmo/state.h>
#include <lib/inspect-vmo/types.h>

namespace inspect {
namespace vmo {

template <>
internal::NumericMetric<int64_t>::~NumericMetric<int64_t>() {
    if (state_) {
        state_->FreeIntMetric(this);
    }
}

template <>
internal::NumericMetric<int64_t>& internal::NumericMetric<int64_t>::
operator=(internal::NumericMetric<int64_t>&& other) {
    if (state_) {
        state_->FreeIntMetric(this);
    }
    state_ = std::move(other.state_);
    name_index_ = std::move(other.name_index_);
    value_index_ = std::move(other.value_index_);
    return *this;
}

template <>
void internal::NumericMetric<int64_t>::Set(int64_t value) {
    if (state_) {
        state_->SetIntMetric(this, value);
    }
}

template <>
void internal::NumericMetric<int64_t>::Add(int64_t value) {
    if (state_) {
        state_->AddIntMetric(this, value);
    }
}

template <>
void internal::NumericMetric<int64_t>::Subtract(int64_t value) {
    if (state_) {
        state_->SubtractIntMetric(this, value);
    }
}

template <>
internal::NumericMetric<uint64_t>::~NumericMetric<uint64_t>() {
    if (state_) {
        state_->FreeUintMetric(this);
    }
}

template <>
internal::NumericMetric<uint64_t>& internal::NumericMetric<uint64_t>::
operator=(internal::NumericMetric<uint64_t>&& other) {
    if (state_) {
        state_->FreeUintMetric(this);
    }
    state_ = std::move(other.state_);
    name_index_ = std::move(other.name_index_);
    value_index_ = std::move(other.value_index_);
    return *this;
}

template <>
void internal::NumericMetric<uint64_t>::Set(uint64_t value) {
    if (state_) {
        state_->SetUintMetric(this, value);
    }
}

template <>
void internal::NumericMetric<uint64_t>::Add(uint64_t value) {
    if (state_) {
        state_->AddUintMetric(this, value);
    }
}

template <>
void internal::NumericMetric<uint64_t>::Subtract(uint64_t value) {
    if (state_) {
        state_->SubtractUintMetric(this, value);
    }
}

template <>
internal::NumericMetric<double>::~NumericMetric<double>() {
    if (state_) {
        state_->FreeDoubleMetric(this);
    }
}

template <>
internal::NumericMetric<double>& internal::NumericMetric<double>::
operator=(internal::NumericMetric<double>&& other) {
    if (state_) {
        state_->FreeDoubleMetric(this);
    }
    state_ = std::move(other.state_);
    name_index_ = std::move(other.name_index_);
    value_index_ = std::move(other.value_index_);
    return *this;
}

template <>
void internal::NumericMetric<double>::Set(double value) {
    if (state_) {
        state_->SetDoubleMetric(this, value);
    }
}

template <>
void internal::NumericMetric<double>::Add(double value) {
    if (state_) {
        state_->AddDoubleMetric(this, value);
    }
}

template <>
void internal::NumericMetric<double>::Subtract(double value) {
    if (state_) {
        state_->SubtractDoubleMetric(this, value);
    }
}

template <>
internal::ArrayValue<int64_t>::~ArrayValue<int64_t>() {
    if (state_) {
        state_->FreeIntArray(this);
    }
}

template <>
internal::ArrayValue<int64_t>& internal::ArrayValue<int64_t>::
operator=(internal::ArrayValue<int64_t>&& other) {
    if (state_) {
        state_->FreeIntArray(this);
    }
    state_ = std::move(other.state_);
    name_index_ = std::move(other.name_index_);
    value_index_ = std::move(other.value_index_);
    return *this;
}

template <>
void internal::ArrayValue<int64_t>::Set(size_t index, int64_t value) {
    if (state_) {
        state_->SetIntArray(this, index, value);
    }
}

template <>
void internal::ArrayValue<int64_t>::Add(size_t index, int64_t value) {
    if (state_) {
        state_->AddIntArray(this, index, value);
    }
}

template <>
void internal::ArrayValue<int64_t>::Subtract(size_t index, int64_t value) {
    if (state_) {
        state_->SubtractIntArray(this, index, value);
    }
}

template <>
internal::ArrayValue<uint64_t>::~ArrayValue<uint64_t>() {
    if (state_) {
        state_->FreeUintArray(this);
    }
}

template <>
internal::ArrayValue<uint64_t>& internal::ArrayValue<uint64_t>::
operator=(internal::ArrayValue<uint64_t>&& other) {
    if (state_) {
        state_->FreeUintArray(this);
    }
    state_ = std::move(other.state_);
    name_index_ = std::move(other.name_index_);
    value_index_ = std::move(other.value_index_);
    return *this;
}

template <>
void internal::ArrayValue<uint64_t>::Set(size_t index, uint64_t value) {
    if (state_) {
        state_->SetUintArray(this, index, value);
    }
}

template <>
void internal::ArrayValue<uint64_t>::Add(size_t index, uint64_t value) {
    if (state_) {
        state_->AddUintArray(this, index, value);
    }
}

template <>
void internal::ArrayValue<uint64_t>::Subtract(size_t index, uint64_t value) {
    if (state_) {
        state_->SubtractUintArray(this, index, value);
    }
}

template <>
internal::ArrayValue<double>::~ArrayValue<double>() {
    if (state_) {
        state_->FreeDoubleArray(this);
    }
}

template <>
internal::ArrayValue<double>& internal::ArrayValue<double>::
operator=(internal::ArrayValue<double>&& other) {
    if (state_) {
        state_->FreeDoubleArray(this);
    }
    state_ = std::move(other.state_);
    name_index_ = std::move(other.name_index_);
    value_index_ = std::move(other.value_index_);
    return *this;
}

template <>
void internal::ArrayValue<double>::Set(size_t index, double value) {
    if (state_) {
        state_->SetDoubleArray(this, index, value);
    }
}

template <>
void internal::ArrayValue<double>::Add(size_t index, double value) {
    if (state_) {
        state_->AddDoubleArray(this, index, value);
    }
}

template <>
void internal::ArrayValue<double>::Subtract(size_t index, double value) {
    if (state_) {
        state_->SubtractDoubleArray(this, index, value);
    }
}

Property::~Property() {
    if (state_) {
        state_->FreeProperty(this);
    }
}

Property& Property::operator=(Property&& other) {
    if (state_) {
        state_->FreeProperty(this);
    }
    state_ = std::move(other.state_);
    name_index_ = std::move(other.name_index_);
    value_index_ = std::move(other.value_index_);
    return *this;
}

void Property::Set(fbl::StringPiece value) {
    if (state_) {
        state_->SetProperty(this, value);
    }
}

Object::~Object() {
    if (state_) {
        state_->FreeObject(this);
    }
}

Object& Object::operator=(Object&& other) {
    if (state_) {
        state_->FreeObject(this);
    }
    state_ = std::move(other.state_);
    name_index_ = std::move(other.name_index_);
    value_index_ = std::move(other.value_index_);
    return *this;
}

Object Object::CreateChild(fbl::StringPiece name) {
    if (state_) {
        return state_->CreateObject(name, value_index_);
    }
    return Object();
}

IntMetric Object::CreateIntMetric(fbl::StringPiece name, int64_t value) {
    if (state_) {
        return state_->CreateIntMetric(name, value_index_, value);
    }
    return IntMetric();
}

UintMetric Object::CreateUintMetric(fbl::StringPiece name, uint64_t value) {
    if (state_) {
        return state_->CreateUintMetric(name, value_index_, value);
    }
    return UintMetric();
}

DoubleMetric Object::CreateDoubleMetric(fbl::StringPiece name, double value) {
    if (state_) {
        return state_->CreateDoubleMetric(name, value_index_, value);
    }
    return DoubleMetric();
}

Property Object::CreateProperty(fbl::StringPiece name, fbl::StringPiece value, PropertyFormat format) {
    if (state_) {
        return state_->CreateProperty(name, value_index_, value, format);
    }
    return Property();
}

IntArray Object::CreateIntArray(fbl::StringPiece name, size_t slots, ArrayFormat format) {
    if (state_) {
        return state_->CreateIntArray(name, value_index_, slots, format);
    }
    return IntArray();
}

UintArray Object::CreateUintArray(fbl::StringPiece name, size_t slots, ArrayFormat format) {
    if (state_) {
        return state_->CreateUintArray(name, value_index_, slots, format);
    }
    return UintArray();
}

DoubleArray Object::CreateDoubleArray(fbl::StringPiece name, size_t slots, ArrayFormat format) {
    if (state_) {
        return state_->CreateDoubleArray(name, value_index_, slots, format);
    }
    return DoubleArray();
}

namespace {
const size_t kExtraSlotsForLinearHistogram = 4;
const size_t kExtraSlotsForExponentialHistogram = 5;
} // namespace

LinearIntHistogram Object::CreateLinearIntHistogram(
    fbl::StringPiece name, int64_t floor, int64_t step_size, size_t buckets) {
    if (state_) {
        const size_t slots = buckets + kExtraSlotsForLinearHistogram;
        auto array = state_->CreateIntArray(
            name, value_index_, slots, ArrayFormat::kLinearHistogram);
        return LinearIntHistogram(floor, step_size, slots, std::move(array));
    }
    return LinearIntHistogram();
}

LinearUintHistogram Object::CreateLinearUintHistogram(
    fbl::StringPiece name, uint64_t floor, uint64_t step_size, size_t buckets) {
    if (state_) {
        const size_t slots = buckets + kExtraSlotsForLinearHistogram;
        auto array = state_->CreateUintArray(
            name, value_index_, slots, ArrayFormat::kLinearHistogram);
        return LinearUintHistogram(floor, step_size, slots, std::move(array));
    }
    return LinearUintHistogram();
}

LinearDoubleHistogram Object::CreateLinearDoubleHistogram(
    fbl::StringPiece name, double floor, double step_size, size_t buckets) {
    if (state_) {
        const size_t slots = buckets + kExtraSlotsForLinearHistogram;
        auto array = state_->CreateDoubleArray(
            name, value_index_, slots, ArrayFormat::kLinearHistogram);
        return LinearDoubleHistogram(floor, step_size, slots, std::move(array));
    }
    return LinearDoubleHistogram();
}

ExponentialIntHistogram Object::CreateExponentialIntHistogram(
    fbl::StringPiece name, int64_t floor, int64_t initial_step, int64_t step_multiplier, size_t buckets) {
    if (state_) {
        const size_t slots = buckets + kExtraSlotsForExponentialHistogram;
        auto array = state_->CreateIntArray(
            name, value_index_, slots, ArrayFormat::kExponentialHistogram);
        return ExponentialIntHistogram(floor, initial_step, step_multiplier, slots, std::move(array));
    }
    return ExponentialIntHistogram();
}

ExponentialUintHistogram Object::CreateExponentialUintHistogram(
    fbl::StringPiece name, uint64_t floor, uint64_t initial_step, uint64_t step_multiplier, size_t buckets) {
    if (state_) {
        const size_t slots = buckets + kExtraSlotsForExponentialHistogram;
        auto array = state_->CreateUintArray(
            name, value_index_, slots, ArrayFormat::kExponentialHistogram);
        return ExponentialUintHistogram(floor, initial_step, step_multiplier, slots, std::move(array));
    }
    return ExponentialUintHistogram();
}

ExponentialDoubleHistogram Object::CreateExponentialDoubleHistogram(
    fbl::StringPiece name, double floor, double initial_step, double step_multiplier, size_t buckets) {
    if (state_) {
        const size_t slots = buckets + kExtraSlotsForExponentialHistogram;
        auto array = state_->CreateDoubleArray(
            name, value_index_, slots, ArrayFormat::kExponentialHistogram);
        return ExponentialDoubleHistogram(floor, initial_step, step_multiplier, slots, std::move(array));
    }
    return ExponentialDoubleHistogram();
}

} // namespace vmo
} // namespace inspect
