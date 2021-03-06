// WARNING: This file is machine generated by fidlgen.

#pragma once

#include <lib/fidl/internal.h>
#include <lib/fidl/cpp/vector_view.h>
#include <lib/fidl/cpp/string_view.h>
#include <lib/fidl/llcpp/array.h>
#include <lib/fidl/llcpp/coding.h>
#include <lib/fidl/llcpp/traits.h>
#include <lib/fidl/llcpp/transaction.h>
#include <lib/fit/function.h>
#include <lib/zx/channel.h>
#include <zircon/fidl.h>

namespace llcpp {

namespace fuchsia {
namespace hardware {
namespace spi {

class Device;

extern "C" const fidl_type_t fuchsia_hardware_spi_DeviceTransmitRequestTable;
extern "C" const fidl_type_t fuchsia_hardware_spi_DeviceTransmitResponseTable;
extern "C" const fidl_type_t fuchsia_hardware_spi_DeviceReceiveRequestTable;
extern "C" const fidl_type_t fuchsia_hardware_spi_DeviceReceiveResponseTable;
extern "C" const fidl_type_t fuchsia_hardware_spi_DeviceExchangeRequestTable;
extern "C" const fidl_type_t fuchsia_hardware_spi_DeviceExchangeResponseTable;

class Device final {
 public:

  struct TransmitResponse final {
    FIDL_ALIGNDECL
    fidl_message_header_t _hdr;
    int32_t status;

    static constexpr const fidl_type_t* Type = &fuchsia_hardware_spi_DeviceTransmitResponseTable;
    static constexpr uint32_t MaxNumHandles = 0;
    static constexpr uint32_t PrimarySize = 24;
    static constexpr uint32_t MaxOutOfLine = 0;
  };
  struct TransmitRequest final {
    FIDL_ALIGNDECL
    fidl_message_header_t _hdr;
    ::fidl::VectorView<uint8_t> data;

    static constexpr const fidl_type_t* Type = &fuchsia_hardware_spi_DeviceTransmitRequestTable;
    static constexpr uint32_t MaxNumHandles = 0;
    static constexpr uint32_t PrimarySize = 32;
    static constexpr uint32_t MaxOutOfLine = 8200;
    using ResponseType = TransmitResponse;
  };

  struct ReceiveResponse final {
    FIDL_ALIGNDECL
    fidl_message_header_t _hdr;
    int32_t status;
    ::fidl::VectorView<uint8_t> data;

    static constexpr const fidl_type_t* Type = &fuchsia_hardware_spi_DeviceReceiveResponseTable;
    static constexpr uint32_t MaxNumHandles = 0;
    static constexpr uint32_t PrimarySize = 40;
    static constexpr uint32_t MaxOutOfLine = 8200;
  };
  struct ReceiveRequest final {
    FIDL_ALIGNDECL
    fidl_message_header_t _hdr;
    uint32_t size;

    static constexpr const fidl_type_t* Type = &fuchsia_hardware_spi_DeviceReceiveRequestTable;
    static constexpr uint32_t MaxNumHandles = 0;
    static constexpr uint32_t PrimarySize = 24;
    static constexpr uint32_t MaxOutOfLine = 0;
    using ResponseType = ReceiveResponse;
  };

  struct ExchangeResponse final {
    FIDL_ALIGNDECL
    fidl_message_header_t _hdr;
    int32_t status;
    ::fidl::VectorView<uint8_t> rxdata;

    static constexpr const fidl_type_t* Type = &fuchsia_hardware_spi_DeviceExchangeResponseTable;
    static constexpr uint32_t MaxNumHandles = 0;
    static constexpr uint32_t PrimarySize = 40;
    static constexpr uint32_t MaxOutOfLine = 8200;
  };
  struct ExchangeRequest final {
    FIDL_ALIGNDECL
    fidl_message_header_t _hdr;
    ::fidl::VectorView<uint8_t> txdata;

    static constexpr const fidl_type_t* Type = &fuchsia_hardware_spi_DeviceExchangeRequestTable;
    static constexpr uint32_t MaxNumHandles = 0;
    static constexpr uint32_t PrimarySize = 32;
    static constexpr uint32_t MaxOutOfLine = 8200;
    using ResponseType = ExchangeResponse;
  };


  class SyncClient final {
   public:
    SyncClient(::zx::channel channel) : channel_(std::move(channel)) {}

    SyncClient(SyncClient&&) = default;

    SyncClient& operator=(SyncClient&&) = default;

    ~SyncClient() {}

    const ::zx::channel& channel() const { return channel_; }

    ::zx::channel* mutable_channel() { return &channel_; }

    // Half-duplex transmit data to a SPI device; always transmits the entire buffer on success.
    zx_status_t Transmit(::fidl::VectorView<uint8_t> data, int32_t* out_status);

    // Half-duplex transmit data to a SPI device; always transmits the entire buffer on success.
    // Caller provides the backing storage for FIDL message via request and response buffers.
    // The lifetime of handles in the response, unless moved, is tied to the returned RAII object.
    ::fidl::DecodeResult<TransmitResponse> Transmit(::fidl::BytePart _request_buffer, ::fidl::VectorView<uint8_t> data, ::fidl::BytePart _response_buffer, int32_t* out_status);

    // Half-duplex transmit data to a SPI device; always transmits the entire buffer on success.
    // Messages are encoded and decoded in-place.
    ::fidl::DecodeResult<TransmitResponse> Transmit(::fidl::DecodedMessage<TransmitRequest> params, ::fidl::BytePart response_buffer);


    // Half-duplex receive data from a SPI device; always reads the full size requested.
    // Caller provides the backing storage for FIDL message via request and response buffers.
    // The lifetime of handles in the response, unless moved, is tied to the returned RAII object.
    ::fidl::DecodeResult<ReceiveResponse> Receive(::fidl::BytePart _request_buffer, uint32_t size, ::fidl::BytePart _response_buffer, int32_t* out_status, ::fidl::VectorView<uint8_t>* out_data);

    // Half-duplex receive data from a SPI device; always reads the full size requested.
    // Messages are encoded and decoded in-place.
    ::fidl::DecodeResult<ReceiveResponse> Receive(::fidl::DecodedMessage<ReceiveRequest> params, ::fidl::BytePart response_buffer);


    // Full-duplex SPI transaction. Received data will exactly equal the length of the transmit
    // buffer.
    // Caller provides the backing storage for FIDL message via request and response buffers.
    // The lifetime of handles in the response, unless moved, is tied to the returned RAII object.
    ::fidl::DecodeResult<ExchangeResponse> Exchange(::fidl::BytePart _request_buffer, ::fidl::VectorView<uint8_t> txdata, ::fidl::BytePart _response_buffer, int32_t* out_status, ::fidl::VectorView<uint8_t>* out_rxdata);

    // Full-duplex SPI transaction. Received data will exactly equal the length of the transmit
    // buffer.
    // Messages are encoded and decoded in-place.
    ::fidl::DecodeResult<ExchangeResponse> Exchange(::fidl::DecodedMessage<ExchangeRequest> params, ::fidl::BytePart response_buffer);

   private:
    ::zx::channel channel_;
  };

  // Methods to make a sync FIDL call directly on an unowned channel, avoiding setting up a client.
  class Call final {
   public:

    // Half-duplex transmit data to a SPI device; always transmits the entire buffer on success.
    static zx_status_t Transmit(zx::unowned_channel _client_end, ::fidl::VectorView<uint8_t> data, int32_t* out_status);

    // Half-duplex transmit data to a SPI device; always transmits the entire buffer on success.
    // Caller provides the backing storage for FIDL message via request and response buffers.
    // The lifetime of handles in the response, unless moved, is tied to the returned RAII object.
    static ::fidl::DecodeResult<TransmitResponse> Transmit(zx::unowned_channel _client_end, ::fidl::BytePart _request_buffer, ::fidl::VectorView<uint8_t> data, ::fidl::BytePart _response_buffer, int32_t* out_status);

    // Half-duplex transmit data to a SPI device; always transmits the entire buffer on success.
    // Messages are encoded and decoded in-place.
    static ::fidl::DecodeResult<TransmitResponse> Transmit(zx::unowned_channel _client_end, ::fidl::DecodedMessage<TransmitRequest> params, ::fidl::BytePart response_buffer);


    // Half-duplex receive data from a SPI device; always reads the full size requested.
    // Caller provides the backing storage for FIDL message via request and response buffers.
    // The lifetime of handles in the response, unless moved, is tied to the returned RAII object.
    static ::fidl::DecodeResult<ReceiveResponse> Receive(zx::unowned_channel _client_end, ::fidl::BytePart _request_buffer, uint32_t size, ::fidl::BytePart _response_buffer, int32_t* out_status, ::fidl::VectorView<uint8_t>* out_data);

    // Half-duplex receive data from a SPI device; always reads the full size requested.
    // Messages are encoded and decoded in-place.
    static ::fidl::DecodeResult<ReceiveResponse> Receive(zx::unowned_channel _client_end, ::fidl::DecodedMessage<ReceiveRequest> params, ::fidl::BytePart response_buffer);


    // Full-duplex SPI transaction. Received data will exactly equal the length of the transmit
    // buffer.
    // Caller provides the backing storage for FIDL message via request and response buffers.
    // The lifetime of handles in the response, unless moved, is tied to the returned RAII object.
    static ::fidl::DecodeResult<ExchangeResponse> Exchange(zx::unowned_channel _client_end, ::fidl::BytePart _request_buffer, ::fidl::VectorView<uint8_t> txdata, ::fidl::BytePart _response_buffer, int32_t* out_status, ::fidl::VectorView<uint8_t>* out_rxdata);

    // Full-duplex SPI transaction. Received data will exactly equal the length of the transmit
    // buffer.
    // Messages are encoded and decoded in-place.
    static ::fidl::DecodeResult<ExchangeResponse> Exchange(zx::unowned_channel _client_end, ::fidl::DecodedMessage<ExchangeRequest> params, ::fidl::BytePart response_buffer);

  };

  // Pure-virtual interface to be implemented by a server.
  class Interface {
   public:
    Interface() = default;
    virtual ~Interface() = default;
    using _Outer = Device;
    using _Base = ::fidl::CompleterBase;

    class TransmitCompleterBase : public _Base {
     public:
      void Reply(int32_t status);
      void Reply(::fidl::BytePart _buffer, int32_t status);
      void Reply(::fidl::DecodedMessage<TransmitResponse> params);

     protected:
      using ::fidl::CompleterBase::CompleterBase;
    };

    using TransmitCompleter = ::fidl::Completer<TransmitCompleterBase>;

    virtual void Transmit(::fidl::VectorView<uint8_t> data, TransmitCompleter::Sync _completer) = 0;

    class ReceiveCompleterBase : public _Base {
     public:
      void Reply(int32_t status, ::fidl::VectorView<uint8_t> data);
      void Reply(::fidl::BytePart _buffer, int32_t status, ::fidl::VectorView<uint8_t> data);
      void Reply(::fidl::DecodedMessage<ReceiveResponse> params);

     protected:
      using ::fidl::CompleterBase::CompleterBase;
    };

    using ReceiveCompleter = ::fidl::Completer<ReceiveCompleterBase>;

    virtual void Receive(uint32_t size, ReceiveCompleter::Sync _completer) = 0;

    class ExchangeCompleterBase : public _Base {
     public:
      void Reply(int32_t status, ::fidl::VectorView<uint8_t> rxdata);
      void Reply(::fidl::BytePart _buffer, int32_t status, ::fidl::VectorView<uint8_t> rxdata);
      void Reply(::fidl::DecodedMessage<ExchangeResponse> params);

     protected:
      using ::fidl::CompleterBase::CompleterBase;
    };

    using ExchangeCompleter = ::fidl::Completer<ExchangeCompleterBase>;

    virtual void Exchange(::fidl::VectorView<uint8_t> txdata, ExchangeCompleter::Sync _completer) = 0;

  };

  // Attempts to dispatch the incoming message to a handler function in the server implementation.
  // If there is no matching handler, it returns false, leaving the message and transaction intact.
  // In all other cases, it consumes the message and returns true.
  // It is possible to chain multiple TryDispatch functions in this manner.
  static bool TryDispatch(Interface* impl, fidl_msg_t* msg, ::fidl::Transaction* txn);

  // Dispatches the incoming message to one of the handlers functions in the interface.
  // If there is no matching handler, it closes all the handles in |msg| and closes the channel with
  // a |ZX_ERR_NOT_SUPPORTED| epitaph, before returning false. The message should then be discarded.
  static bool Dispatch(Interface* impl, fidl_msg_t* msg, ::fidl::Transaction* txn);

  // Same as |Dispatch|, but takes a |void*| instead of |Interface*|. Only used with |fidl::Bind|
  // to reduce template expansion.
  // Do not call this method manually. Use |Dispatch| instead.
  static bool TypeErasedDispatch(void* impl, fidl_msg_t* msg, ::fidl::Transaction* txn) {
    return Dispatch(static_cast<Interface*>(impl), msg, txn);
  }

};

constexpr uint32_t MAX_TRANSFER_SIZE = 8196u;

}  // namespace spi
}  // namespace hardware
}  // namespace fuchsia
}  // namespace llcpp

namespace fidl {

template <>
struct IsFidlType<::llcpp::fuchsia::hardware::spi::Device::TransmitRequest> : public std::true_type {};
template <>
struct IsFidlMessage<::llcpp::fuchsia::hardware::spi::Device::TransmitRequest> : public std::true_type {};
static_assert(sizeof(::llcpp::fuchsia::hardware::spi::Device::TransmitRequest)
    == ::llcpp::fuchsia::hardware::spi::Device::TransmitRequest::PrimarySize);
static_assert(offsetof(::llcpp::fuchsia::hardware::spi::Device::TransmitRequest, data) == 16);

template <>
struct IsFidlType<::llcpp::fuchsia::hardware::spi::Device::TransmitResponse> : public std::true_type {};
template <>
struct IsFidlMessage<::llcpp::fuchsia::hardware::spi::Device::TransmitResponse> : public std::true_type {};
static_assert(sizeof(::llcpp::fuchsia::hardware::spi::Device::TransmitResponse)
    == ::llcpp::fuchsia::hardware::spi::Device::TransmitResponse::PrimarySize);
static_assert(offsetof(::llcpp::fuchsia::hardware::spi::Device::TransmitResponse, status) == 16);

template <>
struct IsFidlType<::llcpp::fuchsia::hardware::spi::Device::ReceiveRequest> : public std::true_type {};
template <>
struct IsFidlMessage<::llcpp::fuchsia::hardware::spi::Device::ReceiveRequest> : public std::true_type {};
static_assert(sizeof(::llcpp::fuchsia::hardware::spi::Device::ReceiveRequest)
    == ::llcpp::fuchsia::hardware::spi::Device::ReceiveRequest::PrimarySize);
static_assert(offsetof(::llcpp::fuchsia::hardware::spi::Device::ReceiveRequest, size) == 16);

template <>
struct IsFidlType<::llcpp::fuchsia::hardware::spi::Device::ReceiveResponse> : public std::true_type {};
template <>
struct IsFidlMessage<::llcpp::fuchsia::hardware::spi::Device::ReceiveResponse> : public std::true_type {};
static_assert(sizeof(::llcpp::fuchsia::hardware::spi::Device::ReceiveResponse)
    == ::llcpp::fuchsia::hardware::spi::Device::ReceiveResponse::PrimarySize);
static_assert(offsetof(::llcpp::fuchsia::hardware::spi::Device::ReceiveResponse, status) == 16);
static_assert(offsetof(::llcpp::fuchsia::hardware::spi::Device::ReceiveResponse, data) == 24);

template <>
struct IsFidlType<::llcpp::fuchsia::hardware::spi::Device::ExchangeRequest> : public std::true_type {};
template <>
struct IsFidlMessage<::llcpp::fuchsia::hardware::spi::Device::ExchangeRequest> : public std::true_type {};
static_assert(sizeof(::llcpp::fuchsia::hardware::spi::Device::ExchangeRequest)
    == ::llcpp::fuchsia::hardware::spi::Device::ExchangeRequest::PrimarySize);
static_assert(offsetof(::llcpp::fuchsia::hardware::spi::Device::ExchangeRequest, txdata) == 16);

template <>
struct IsFidlType<::llcpp::fuchsia::hardware::spi::Device::ExchangeResponse> : public std::true_type {};
template <>
struct IsFidlMessage<::llcpp::fuchsia::hardware::spi::Device::ExchangeResponse> : public std::true_type {};
static_assert(sizeof(::llcpp::fuchsia::hardware::spi::Device::ExchangeResponse)
    == ::llcpp::fuchsia::hardware::spi::Device::ExchangeResponse::PrimarySize);
static_assert(offsetof(::llcpp::fuchsia::hardware::spi::Device::ExchangeResponse, status) == 16);
static_assert(offsetof(::llcpp::fuchsia::hardware::spi::Device::ExchangeResponse, rxdata) == 24);

}  // namespace fidl
