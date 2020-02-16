// This software is distributed under the terms of the MIT License.
// Copyright (c) 2016-2020 UAVCAN Development Team.

#include "canard.h"
#include <assert.h>
#include <string.h>

// ---------------------------------------- BUILD CONFIGURATION ----------------------------------------

/// By default, this macro resolves to the standard assert(). The user can redefine this if necessary.
/// To disable assertion checks completely, make it expand into `(void)(0)`.
#ifndef CANARD_ASSERT
// Intentional violation of MISRA: assertion macro cannot be replaced with a function definition.
#    define CANARD_ASSERT(x) assert(x)  // NOSONAR
#endif

/// This macro is needed only for testing and for library development. Do not redefine this in production.
#if defined(CANARD_EXPOSE_INTERNALS) && CANARD_EXPOSE_INTERNALS
#    define CANARD_INTERNAL
#else
#    define CANARD_INTERNAL static inline
#endif

#if !defined(__STDC_VERSION__) || (__STDC_VERSION__ < 199901L)
#    error "Unsupported language: ISO C99 or a newer version is required."
#endif

#ifndef static_assert
// Intentional violation of MISRA: static assertion macro cannot be replaced with a function definition.
#    define static_assert(x, ...) typedef char _static_assert_gl(_static_assertion_, __LINE__)[(x) ? 1 : -1]  // NOSONAR
#    define _static_assert_gl(a, b) _static_assert_gl_impl(a, b)                                              // NOSONAR
// Intentional violation of MISRA: the paste operator ## cannot be avoided in this context.
#    define _static_assert_gl_impl(a, b) a##b  // NOSONAR
#endif

// ---------------------------------------- COMMON CONSTANTS ----------------------------------------

#define TAIL_START_OF_TRANSFER 128U
#define TAIL_END_OF_TRANSFER 64U
#define TAIL_TOGGLE 32U

#define CAN_EXT_ID_MASK ((UINT32_C(1) << 29U) - 1U)

#define BITS_PER_BYTE 8U
#define BYTE_MAX 0xFFU

#define PADDING_BYTE 0U

// ---------------------------------------- TRANSFER CRC ----------------------------------------

typedef uint16_t TransferCRC;

#define CRC_INITIAL 0xFFFFU
#define CRC_SIZE_BYTES 2U

CANARD_INTERNAL TransferCRC crcAddByte(const TransferCRC crc, const uint8_t byte);
CANARD_INTERNAL TransferCRC crcAddByte(const TransferCRC crc, const uint8_t byte)
{
    TransferCRC out = crc ^ (uint16_t)((uint16_t)(byte) << BITS_PER_BYTE);
    for (uint8_t i = 0; i < BITS_PER_BYTE; i++)  // Should we use a table instead? Adds 512 bytes of ROM.
    {
        // The no-lint statements suppress the warnings about magic numbers. These numbers are not magic.
        out = ((out & 0x8000U) != 0U) ? ((uint16_t)(out << 1U) ^ 0x1021U) : (uint16_t)(out << 1U);  // NOLINT
    }
    return out;
}

CANARD_INTERNAL TransferCRC crcAdd(const TransferCRC crc, const size_t size, const void* const data);
CANARD_INTERNAL TransferCRC crcAdd(const TransferCRC crc, const size_t size, const void* const data)
{
    CANARD_ASSERT((data != NULL) || (size == 0U));
    TransferCRC    out = crc;
    const uint8_t* p   = (const uint8_t*) data;
    for (size_t i = 0; i < size; i++)
    {
        out = crcAddByte(out, *p);
        ++p;
    }
    return out;
}

// ---------------------------------------- SESSION SPECIFIER ----------------------------------------

#define OFFSET_PRIORITY 26U
#define OFFSET_SUBJECT_ID 8U
#define OFFSET_SERVICE_ID 14U
#define OFFSET_DST_NODE_ID 7U

#define FLAG_SERVICE_NOT_MESSAGE (UINT32_C(1) << 25U)
#define FLAG_ANONYMOUS_MESSAGE (UINT32_C(1) << 24U)
#define FLAG_REQUEST_NOT_RESPONSE (UINT32_C(1) << 24U)
#define FLAG_RESERVED_23 (UINT32_C(1) << 23U)
#define FLAG_RESERVED_07 (UINT32_C(1) << 7U)

CANARD_INTERNAL uint32_t makeMessageSessionSpecifier(const CanardPortID subject_id, const CanardNodeID src_node_id);
CANARD_INTERNAL uint32_t makeMessageSessionSpecifier(const CanardPortID subject_id, const CanardNodeID src_node_id)
{
    CANARD_ASSERT(src_node_id <= CANARD_NODE_ID_MAX);
    CANARD_ASSERT(subject_id <= CANARD_SUBJECT_ID_MAX);
    return src_node_id | ((uint32_t) subject_id << OFFSET_SUBJECT_ID);
}

CANARD_INTERNAL uint32_t makeServiceSessionSpecifier(const CanardPortID service_id,
                                                     const bool         request_not_response,
                                                     const CanardNodeID src_node_id,
                                                     const CanardNodeID dst_node_id);
CANARD_INTERNAL uint32_t makeServiceSessionSpecifier(const CanardPortID service_id,
                                                     const bool         request_not_response,
                                                     const CanardNodeID src_node_id,
                                                     const CanardNodeID dst_node_id)
{
    CANARD_ASSERT(src_node_id <= CANARD_NODE_ID_MAX);
    CANARD_ASSERT(dst_node_id <= CANARD_NODE_ID_MAX);
    CANARD_ASSERT(service_id <= CANARD_SERVICE_ID_MAX);
    return src_node_id | (((uint32_t) dst_node_id) << OFFSET_DST_NODE_ID) |  //
           (((uint32_t) service_id) << OFFSET_SERVICE_ID) |                  //
           (request_not_response ? FLAG_REQUEST_NOT_RESPONSE : 0U) | FLAG_SERVICE_NOT_MESSAGE;
}

// ---------------------------------------- TRANSMISSION ----------------------------------------

/// The memory requirement model provided in the documentation assumes that the maximum size of this structure never
/// exceeds 32 bytes on any conventional platform. The sizeof() of this structure, per the C standard, assumes that
/// the length of the flex array member is zero.
/// A user that needs a detailed analysis of the worst-case memory consumption may compute the size of this structure
/// for the particular platform at hand manually or by evaluating its sizeof().
/// The fields are ordered to minimize the amount of padding on all conventional platforms.
typedef struct CanardInternalTxQueueItem
{
    struct CanardInternalTxQueueItem* next;

    CanardMicrosecond deadline_usec;
    size_t            payload_size;
    uint32_t          id;

    // Intentional violation of MISRA: this flex array is the lesser of three evils. The other two are:
    //  - Use pointer, make it point to the remainder of the allocated memory following this structure.
    //    The pointer is bad because it requires us to use pointer arithmetics and adds sizeof(void*) of waste per item.
    //  - Use a separate memory allocation for data. This is terribly wasteful (both time & memory).
    uint8_t payload[];  // NOSONAR
} CanardInternalTxQueueItem;

/// This is the transport MTU rounded up to next full DLC minus the tail byte.
CANARD_INTERNAL size_t getPresentationLayerMTU(const CanardInstance* const ins);
CANARD_INTERNAL size_t getPresentationLayerMTU(const CanardInstance* const ins)
{
    const size_t max_index = (sizeof(CanardCANLengthToDLC) / sizeof(CanardCANLengthToDLC[0])) - 1U;
    size_t       mtu       = 0U;
    if (ins->mtu_bytes < CANARD_MTU_CAN_CLASSIC)
    {
        mtu = CANARD_MTU_CAN_CLASSIC;
    }
    else if (ins->mtu_bytes <= max_index)
    {
        mtu = CanardCANDLCToLength[CanardCANLengthToDLC[ins->mtu_bytes]];  // Round up to nearest valid length.
    }
    else
    {
        mtu = CanardCANDLCToLength[CanardCANLengthToDLC[max_index]];
    }
    return mtu - 1U;
}

CANARD_INTERNAL int32_t makeCANID(const CanardTransfer* const tr,
                                  const CanardNodeID          local_node_id,
                                  const size_t                presentation_layer_mtu);
CANARD_INTERNAL int32_t makeCANID(const CanardTransfer* const tr,
                                  const CanardNodeID          local_node_id,
                                  const size_t                presentation_layer_mtu)
{
    CANARD_ASSERT(tr != NULL);
    CANARD_ASSERT(presentation_layer_mtu > 0);
    int32_t out = -CANARD_ERROR_INVALID_ARGUMENT;
    if ((tr->transfer_kind == CanardTransferKindMessage) && (CANARD_NODE_ID_UNSET == tr->remote_node_id) &&
        (tr->port_id <= CANARD_SUBJECT_ID_MAX))
    {
        if (local_node_id <= CANARD_NODE_ID_MAX)
        {
            out = (int32_t) makeMessageSessionSpecifier(tr->port_id, local_node_id);
            CANARD_ASSERT(out >= 0);
        }
        else if (tr->payload_size <= presentation_layer_mtu)
        {
            CANARD_ASSERT((tr->payload != NULL) || (tr->payload_size == 0U));
            const CanardNodeID c =
                (CanardNodeID)(crcAdd(CRC_INITIAL, tr->payload_size, tr->payload) & CANARD_NODE_ID_MAX);
            const uint32_t spec = makeMessageSessionSpecifier(tr->port_id, c) | FLAG_ANONYMOUS_MESSAGE;
            CANARD_ASSERT(spec <= CAN_EXT_ID_MASK);
            out = (int32_t) spec;
        }
        else
        {
            out = -CANARD_ERROR_INVALID_ARGUMENT;  // Anonymous multi-frame message trs are not allowed.
        }
    }
    else if (((tr->transfer_kind == CanardTransferKindRequest) || (tr->transfer_kind == CanardTransferKindResponse)) &&
             (tr->remote_node_id <= CANARD_NODE_ID_MAX) && (tr->port_id <= CANARD_SERVICE_ID_MAX))
    {
        if (local_node_id <= CANARD_NODE_ID_MAX)
        {
            out = (int32_t) makeServiceSessionSpecifier(tr->port_id,
                                                        tr->transfer_kind == CanardTransferKindRequest,
                                                        local_node_id,
                                                        tr->remote_node_id);
            CANARD_ASSERT(out >= 0);
        }
        else
        {
            out = -CANARD_ERROR_INVALID_ARGUMENT;  // Anonymous service transfers are not allowed.
        }
    }
    else
    {
        out = -CANARD_ERROR_INVALID_ARGUMENT;
    }

    if (out >= 0)
    {
        const uint32_t prio = (uint32_t) tr->priority;
        if (prio <= CANARD_PRIORITY_MAX)
        {
            const uint32_t id = ((uint32_t) out) | (prio << OFFSET_PRIORITY);
            out               = (int32_t) id;
        }
        else
        {
            out = -CANARD_ERROR_INVALID_ARGUMENT;
        }
    }
    return out;
}

CANARD_INTERNAL uint8_t makeTailByte(const bool             start_of_transfer,
                                     const bool             end_of_transfer,
                                     const bool             toggle,
                                     const CanardTransferID transfer_id);
CANARD_INTERNAL uint8_t makeTailByte(const bool             start_of_transfer,
                                     const bool             end_of_transfer,
                                     const bool             toggle,
                                     const CanardTransferID transfer_id)
{
    CANARD_ASSERT(start_of_transfer ? toggle : true);
    return (uint8_t)((start_of_transfer ? TAIL_START_OF_TRANSFER : 0U) | (end_of_transfer ? TAIL_END_OF_TRANSFER : 0U) |
                     (toggle ? TAIL_TOGGLE : 0U) | (transfer_id & CANARD_TRANSFER_ID_MAX));
}

/// Takes a frame payload size, returns a new size that is >=x and is rounded up to the nearest valid DLC.
CANARD_INTERNAL size_t roundFramePayloadSizeUp(const size_t x);
CANARD_INTERNAL size_t roundFramePayloadSizeUp(const size_t x)
{
    CANARD_ASSERT(x < (sizeof(CanardCANLengthToDLC) / sizeof(CanardCANLengthToDLC[0])));
    // Suppressing a false-positive out-of-bounds access error from Sonar. Its control flow analyser is misbehaving.
    const size_t y = CanardCANLengthToDLC[x];  // NOSONAR
    CANARD_ASSERT(y < (sizeof(CanardCANDLCToLength) / sizeof(CanardCANDLCToLength[0])));
    return CanardCANDLCToLength[y];
}

CANARD_INTERNAL CanardInternalTxQueueItem* allocateTxQueueItem(CanardInstance* const   ins,
                                                               const uint32_t          id,
                                                               const CanardMicrosecond deadline_usec,
                                                               const size_t            payload_size);
CANARD_INTERNAL CanardInternalTxQueueItem* allocateTxQueueItem(CanardInstance* const   ins,
                                                               const uint32_t          id,
                                                               const CanardMicrosecond deadline_usec,
                                                               const size_t            payload_size)
{
    CANARD_ASSERT(ins != NULL);
    CANARD_ASSERT(payload_size > 0U);
    CanardInternalTxQueueItem* const out =
        (CanardInternalTxQueueItem*) ins->memory_allocate(ins, sizeof(CanardInternalTxQueueItem) + payload_size);
    if (out != NULL)
    {
        out->next          = NULL;
        out->deadline_usec = deadline_usec;
        out->payload_size  = payload_size;
        out->id            = id;
    }
    return out;
}

/// Returns the element after which new elements with the specified CAN ID should be inserted.
/// Returns NULL if the element shall be inserted in the beginning of the list (i.e., no prior elements).
CANARD_INTERNAL CanardInternalTxQueueItem* findTxQueueSupremum(const CanardInstance* const ins, const uint32_t can_id);
CANARD_INTERNAL CanardInternalTxQueueItem* findTxQueueSupremum(const CanardInstance* const ins, const uint32_t can_id)
{
    CANARD_ASSERT(ins != NULL);
    CANARD_ASSERT(can_id <= CAN_EXT_ID_MASK);
    CanardInternalTxQueueItem* out = ins->_tx_queue;
    if ((NULL == out) || (out->id > can_id))
    {
        out = NULL;
    }
    else
    {
        // TODO The linear search should be replaced with O(log n) at least. Please help us here.
        while ((out != NULL) && (out->next != NULL) && (out->next->id <= can_id))
        {
            out = out->next;
        }
    }
    CANARD_ASSERT((out == NULL) || (out->id <= can_id));
    return out;
}

/// Returns the number of frames enqueued or error (i.e., =1 or <0).
CANARD_INTERNAL int32_t pushSingleFrameTransfer(CanardInstance* const   ins,
                                                const CanardMicrosecond deadline_usec,
                                                const uint32_t          can_id,
                                                const CanardTransferID  transfer_id,
                                                const size_t            payload_size,
                                                const void* const       payload);
CANARD_INTERNAL int32_t pushSingleFrameTransfer(CanardInstance* const   ins,
                                                const CanardMicrosecond deadline_usec,
                                                const uint32_t          can_id,
                                                const CanardTransferID  transfer_id,
                                                const size_t            payload_size,
                                                const void* const       payload)
{
    CANARD_ASSERT(ins != NULL);
    CANARD_ASSERT((payload != NULL) || (payload_size == 0));

    const size_t frame_payload_size = roundFramePayloadSizeUp(payload_size + 1U);
    CANARD_ASSERT(frame_payload_size > payload_size);
    const size_t padding_size = frame_payload_size - payload_size - 1U;
    CANARD_ASSERT((padding_size + payload_size + 1U) == frame_payload_size);
    int32_t out = 0;

    CanardInternalTxQueueItem* const tqi = allocateTxQueueItem(ins, can_id, deadline_usec, frame_payload_size);
    if (tqi != NULL)
    {
        if (payload_size > 0U)  // The check is needed to avoid calling memcpy() with a NULL pointer, it's an UB.
        {
            CANARD_ASSERT(payload != NULL);
            // Clang-Tidy raises an error recommending the use of memcpy_s() instead.
            // We ignore this recommendation because it is not available in C99.
            (void) memcpy(&tqi->payload[0], payload, payload_size);  // NOLINT
        }

        // Clang-Tidy raises an error recommending the use of memset_s() instead.
        // We ignore this recommendation because it is not available in C99.
        (void) memset(&tqi->payload[payload_size], PADDING_BYTE, padding_size);  // NOLINT

        tqi->payload[frame_payload_size - 1U] = makeTailByte(true, true, true, transfer_id);
        CanardInternalTxQueueItem* const sup  = findTxQueueSupremum(ins, can_id);
        if (sup != NULL)
        {
            tqi->next = sup->next;
            sup->next = tqi;
        }
        else
        {
            tqi->next      = ins->_tx_queue;
            ins->_tx_queue = tqi;
        }
        out = 1;  // One frame enqueued.
    }
    else
    {
        out = -CANARD_ERROR_OUT_OF_MEMORY;
    }
    CANARD_ASSERT((out < 0) || (out == 1));
    return out;
}

/// Returns the number of frames enqueued or error.
CANARD_INTERNAL int32_t pushMultiFrameTransfer(CanardInstance* const   ins,
                                               const size_t            presentation_layer_mtu,
                                               const CanardMicrosecond deadline_usec,
                                               const uint32_t          can_id,
                                               const CanardTransferID  transfer_id,
                                               const size_t            payload_size,
                                               const void* const       payload);
CANARD_INTERNAL int32_t pushMultiFrameTransfer(CanardInstance* const   ins,
                                               const size_t            presentation_layer_mtu,
                                               const CanardMicrosecond deadline_usec,
                                               const uint32_t          can_id,
                                               const CanardTransferID  transfer_id,
                                               const size_t            payload_size,
                                               const void* const       payload)
{
    CANARD_ASSERT(ins != NULL);
    CANARD_ASSERT(presentation_layer_mtu > 0U);
    CANARD_ASSERT(payload_size > presentation_layer_mtu);  // Otherwise, a single-frame transfer should be used.
    CANARD_ASSERT(payload != NULL);

    int32_t out = 0;  // The number of frames enqueued or negated error.

    CanardInternalTxQueueItem* head = NULL;  // Head and tail of the linked list of frames of this transfer.
    CanardInternalTxQueueItem* tail = NULL;

    const size_t   payload_size_with_crc = payload_size + CRC_SIZE_BYTES;
    size_t         offset                = 0U;
    TransferCRC    crc                   = crcAdd(CRC_INITIAL, payload_size, payload);
    bool           start_of_transfer     = true;
    bool           toggle                = true;
    const uint8_t* payload_ptr           = (const uint8_t*) payload;

    while (offset < payload_size_with_crc)
    {
        ++out;
        const size_t frame_payload_size_with_tail =
            ((payload_size_with_crc - offset) < presentation_layer_mtu)
                ? roundFramePayloadSizeUp((payload_size_with_crc - offset) + 1U)  // Add padding only in the last frame.
                : (presentation_layer_mtu + 1U);
        CanardInternalTxQueueItem* const tqi =
            allocateTxQueueItem(ins, can_id, deadline_usec, frame_payload_size_with_tail);
        if (NULL == head)
        {
            head = tqi;
        }
        else
        {
            tail->next = tqi;
        }
        tail = tqi;
        if (NULL == tail)
        {
            break;
        }

        // Copy the payload into the frame.
        const size_t frame_payload_size = frame_payload_size_with_tail - 1U;
        size_t       frame_offset       = 0U;
        if (offset < payload_size)
        {
            size_t move_size = payload_size - offset;
            if (move_size > frame_payload_size)
            {
                move_size = frame_payload_size;
            }
            // Clang-Tidy raises an error recommending the use of memcpy_s() instead.
            // We ignore this recommendation because it is not available in C99.
            (void) memcpy(&tail->payload[0], payload_ptr, move_size);  // NOLINT
            frame_offset = frame_offset + move_size;
            offset += move_size;
            payload_ptr += move_size;
        }

        // Handle the last frame of the transfer: it is special because it also contains padding and CRC.
        if (offset >= payload_size)
        {
            // Insert padding -- only in the last frame. Don't forget to include padding into the CRC.
            while ((frame_offset + CRC_SIZE_BYTES) < frame_payload_size)
            {
                tail->payload[frame_offset] = PADDING_BYTE;
                ++frame_offset;
                crc = crcAddByte(crc, PADDING_BYTE);
            }

            // Insert the CRC.
            if ((frame_offset < frame_payload_size) && (offset == payload_size))
            {
                tail->payload[frame_offset] = (uint8_t)(crc >> BITS_PER_BYTE);
                ++frame_offset;
                ++offset;
            }
            if ((frame_offset < frame_payload_size) && (offset > payload_size))
            {
                tail->payload[frame_offset] = (uint8_t)(crc & BYTE_MAX);
                ++frame_offset;
                ++offset;
            }
        }

        // Finalize the frame.
        CANARD_ASSERT((frame_offset + 1U) == tail->payload_size);
        tail->payload[frame_offset] =
            makeTailByte(start_of_transfer, offset >= payload_size_with_crc, toggle, transfer_id);
        start_of_transfer = false;
        toggle            = !toggle;
    }

    if (tail != NULL)
    {
        CANARD_ASSERT(head->next != NULL);  // This is not a single-frame transfer so at least two frames shall exist.
        CANARD_ASSERT(tail->next == NULL);  // The list shall be properly terminated.
        CanardInternalTxQueueItem* const sup = findTxQueueSupremum(ins, can_id);
        if (NULL == sup)  // Once the insertion point is located, we insert the entire frame sequence in constant time.
        {
            tail->next     = ins->_tx_queue;
            ins->_tx_queue = head;
        }
        else
        {
            tail->next = sup->next;
            sup->next  = head;
        }
    }
    else  // Failed to allocate at least one frame in the queue! Remove all frames and abort.
    {
        out = -CANARD_ERROR_OUT_OF_MEMORY;
        while (head != NULL)
        {
            CanardInternalTxQueueItem* const next = head->next;
            ins->memory_free(ins, head);
            head = next;
        }
    }

    CANARD_ASSERT((out < 0) || (out >= 2));
    return out;
}

// ---------------------------------------- RECEPTION ----------------------------------------

#define SESSIONS_PER_SUBSCRIPTION (CANARD_NODE_ID_MAX + 1U)

/// The memory requirement model provided in the documentation assumes that the maximum size of this structure never
/// exceeds 32 bytes on any conventional platform.
/// A user that needs a detailed analysis of the worst-case memory consumption may compute the size of this structure
/// for the particular platform at hand manually or by evaluating its sizeof().
/// The fields are ordered to minimize the amount of padding on all conventional platforms.
typedef struct CanardInternalRxSession
{
    CanardMicrosecond transfer_timestamp_usec;  ///< Timestamp of the last received start-of-transfer.
    size_t            payload_size;             ///< How many bytes received so far.
    uint8_t*          payload;                  ///< Dynamically allocated and handed off to the application when done.
    TransferCRC       calculated_crc;           ///< Updated with the received payload in real time.
    CanardTransferID  toggle_and_transfer_id;   ///< Toggle and transfer-ID combined into one field to reduce footprint.
    uint8_t           iface_index;              ///< Arbitrary value in [0, 255].
} CanardInternalRxSession;

/// High-level transport frame model.
typedef struct
{
    CanardMicrosecond timestamp_usec;

    CanardPriority priority;

    CanardTransferKind transfer_kind;
    CanardPortID       port_id;
    CanardNodeID       source_node_id;
    CanardNodeID       destination_node_id;

    CanardTransferID transfer_id;
    bool             start_of_transfer;
    bool             end_of_transfer;
    bool             toggle;

    size_t         payload_size;
    const uint8_t* payload;
} FrameModel;

/// Returns truth if the frame is valid and parsed successfully. False if the frame is not a valid UAVCAN/CAN frame.
CANARD_INTERNAL bool tryParseFrame(const CanardFrame* const frame, FrameModel* const out_result);
CANARD_INTERNAL bool tryParseFrame(const CanardFrame* const frame, FrameModel* const out_result)
{
    CANARD_ASSERT(frame != NULL);
    CANARD_ASSERT(frame->extended_can_id <= CAN_EXT_ID_MASK);
    CANARD_ASSERT(out_result != NULL);
    bool valid = false;
    if (frame->payload_size > 0)
    {
        CANARD_ASSERT(frame->payload != NULL);
        out_result->timestamp_usec = frame->timestamp_usec;

        // CAN ID parsing.
        const uint32_t can_id      = frame->extended_can_id;
        out_result->priority       = (CanardPriority)((can_id >> OFFSET_PRIORITY) & CANARD_PRIORITY_MAX);
        out_result->source_node_id = (CanardNodeID)(can_id & CANARD_NODE_ID_MAX);
        if (0 == (can_id & FLAG_SERVICE_NOT_MESSAGE))
        {
            valid                     = (0 == (can_id & FLAG_RESERVED_23)) && (0 == (can_id & FLAG_RESERVED_07));
            out_result->transfer_kind = CanardTransferKindMessage;
            out_result->port_id       = (CanardPortID)((can_id >> OFFSET_SUBJECT_ID) & CANARD_SUBJECT_ID_MAX);
            if ((can_id & FLAG_ANONYMOUS_MESSAGE) != 0)
            {
                out_result->source_node_id = CANARD_NODE_ID_UNSET;
            }
            out_result->destination_node_id = CANARD_NODE_ID_UNSET;
        }
        else
        {
            valid = (0 == (can_id & FLAG_RESERVED_23));
            out_result->transfer_kind =
                ((can_id & FLAG_REQUEST_NOT_RESPONSE) != 0) ? CanardTransferKindRequest : CanardTransferKindResponse;
            out_result->port_id             = (CanardPortID)((can_id >> OFFSET_SERVICE_ID) & CANARD_SERVICE_ID_MAX);
            out_result->destination_node_id = (CanardNodeID)((can_id >> OFFSET_DST_NODE_ID) & CANARD_NODE_ID_MAX);
        }

        // Payload parsing.
        out_result->payload_size = frame->payload_size - 1U;  // Cut off the tail byte.
        out_result->payload      = (const uint8_t*) frame->payload;

        // Tail byte parsing.
        // Intentional MISRA violation: indexing on a pointer. This is done to avoid pointer arithmetics.
        const uint8_t tail            = out_result->payload[out_result->payload_size];
        out_result->transfer_id       = tail & CANARD_TRANSFER_ID_MAX;
        out_result->start_of_transfer = ((tail & TAIL_START_OF_TRANSFER) != 0);
        out_result->end_of_transfer   = ((tail & TAIL_END_OF_TRANSFER) != 0);
        out_result->toggle            = ((tail & TAIL_TOGGLE) != 0);

        // Final validation.
        valid = valid && (out_result->start_of_transfer ? out_result->toggle : true);  // Protocol version check.
        valid = valid && ((CANARD_NODE_ID_UNSET == out_result->source_node_id)
                              ? (out_result->start_of_transfer && out_result->end_of_transfer)  // Single-frame.
                              : true);
    }
    return valid;
}

CANARD_INTERNAL void initRxTransferFromFrame(const FrameModel* const frame, CanardTransfer* const out_transfer);
CANARD_INTERNAL void initRxTransferFromFrame(const FrameModel* const frame, CanardTransfer* const out_transfer)
{
    CANARD_ASSERT(frame != NULL);
    CANARD_ASSERT(frame->payload != NULL);
    CANARD_ASSERT(out_transfer != NULL);
    out_transfer->timestamp_usec = frame->timestamp_usec;
    out_transfer->priority       = frame->priority;
    out_transfer->transfer_kind  = frame->transfer_kind;
    out_transfer->port_id        = frame->port_id;
    out_transfer->remote_node_id = frame->source_node_id;
    out_transfer->transfer_id    = frame->transfer_id;
    out_transfer->payload_size   = frame->payload_size;
    out_transfer->payload        = frame->payload;
}

CANARD_INTERNAL int8_t updateRxSession(CanardInstance* const          ins,
                                       CanardInternalRxSession* const rxs,
                                       const FrameModel* const        frame,
                                       const uint8_t                  iface_index,
                                       const CanardMicrosecond        transfer_id_timeout_usec,
                                       const size_t                   payload_size_bytes_max,
                                       CanardTransfer* const          out_transfer);
CANARD_INTERNAL int8_t updateRxSession(CanardInstance* const          ins,
                                       CanardInternalRxSession* const rxs,
                                       const FrameModel* const        frame,
                                       const uint8_t                  iface_index,
                                       const CanardMicrosecond        transfer_id_timeout_usec,
                                       const size_t                   payload_size_bytes_max,
                                       CanardTransfer* const          out_transfer)
{
    CANARD_ASSERT(ins != NULL);
    CANARD_ASSERT(rxs != NULL);
    CANARD_ASSERT(frame != NULL);
    CANARD_ASSERT(frame->payload != NULL);
    CANARD_ASSERT(out_transfer != NULL);

    int8_t out = 0;

    (void) iface_index;
    (void) transfer_id_timeout_usec;
    (void) payload_size_bytes_max;

    return out;
}

CANARD_INTERNAL int8_t acceptFrame(CanardInstance* const   ins,
                                   const FrameModel* const frame,
                                   const uint8_t           iface_index,
                                   CanardTransfer* const   out_transfer);
CANARD_INTERNAL int8_t acceptFrame(CanardInstance* const   ins,
                                   const FrameModel* const frame,
                                   const uint8_t           iface_index,
                                   CanardTransfer* const   out_transfer)
{
    CANARD_ASSERT(ins != NULL);
    CANARD_ASSERT(frame != NULL);
    CANARD_ASSERT(frame->payload != NULL);
    CANARD_ASSERT((CANARD_NODE_ID_UNSET == frame->destination_node_id) || (ins->node_id == frame->destination_node_id));
    CANARD_ASSERT(out_transfer != NULL);

    // Find subscription. This is the reason the function has a linear time complexity from the number of subscriptions.
    CanardRxSubscription* sub = ins->_rx_subscriptions[(size_t) frame->transfer_kind];
    while ((sub != NULL) && (sub->_port_id != frame->port_id))
    {
        sub = sub->_next;
    }

    // If the subscription is not found, that means that the application doesn't want this transfer. Ignore the frame.
    int8_t out = 0;
    if (sub != NULL)
    {
        CANARD_ASSERT(sub->_port_id == frame->port_id);
        if (frame->source_node_id <= CANARD_NODE_ID_MAX)
        {
            // If such session does not exist, create it. This only makes sense if this is the first frame of a
            // transfer, otherwise, we won't be able to receive the transfer anyway so we don't bother.
            if ((NULL == sub->_sessions[frame->source_node_id]) && frame->start_of_transfer)
            {
                CanardInternalRxSession* const rxs =
                    (CanardInternalRxSession*) ins->memory_allocate(ins,
                                                                    sizeof(CanardInternalRxSession) +
                                                                        sub->_payload_size_bytes_max);
                sub->_sessions[frame->source_node_id] = rxs;
                if (rxs != NULL)
                {
                    rxs->transfer_timestamp_usec = frame->timestamp_usec;
                    rxs->payload_size            = 0U;
                    rxs->payload                 = NULL;
                    rxs->calculated_crc          = CRC_INITIAL;
                    rxs->toggle_and_transfer_id  = TAIL_TOGGLE;
                    rxs->iface_index             = 0U;
                }
                else
                {
                    out = -CANARD_ERROR_OUT_OF_MEMORY;
                }
            }
            // There are two possible reasons why the session may not exist: 1. OOM; 2. SOT-miss.
            if (sub->_sessions[frame->source_node_id] != NULL)
            {
                CANARD_ASSERT(out == 0);
                out = updateRxSession(ins,
                                      sub->_sessions[frame->source_node_id],
                                      frame,
                                      iface_index,
                                      sub->_transfer_id_timeout_usec,
                                      sub->_payload_size_bytes_max,
                                      out_transfer);
            }
        }
        else
        {
            CANARD_ASSERT(frame->source_node_id == CANARD_NODE_ID_UNSET);
            // Anonymous transfers are stateless. No need to update the state machine, just blindly accept it.
            initRxTransferFromFrame(frame, out_transfer);
            out = 1;
        }
    }
    CANARD_ASSERT(out <= 1);
    return out;
}

// ---------------------------------------- PUBLIC API ----------------------------------------

const uint8_t CanardCANDLCToLength[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64};
const uint8_t CanardCANLengthToDLC[65] = {
    0,  1,  2,  3,  4,  5,  6,  7,  8,                               // 0-8
    9,  9,  9,  9,                                                   // 9-12
    10, 10, 10, 10,                                                  // 13-16
    11, 11, 11, 11,                                                  // 17-20
    12, 12, 12, 12,                                                  // 21-24
    13, 13, 13, 13, 13, 13, 13, 13,                                  // 25-32
    14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,  // 33-48
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,  // 49-64
};

CanardInstance canardInit(const CanardMemoryAllocate memory_allocate, const CanardMemoryFree memory_free)
{
    CANARD_ASSERT(memory_allocate != NULL);
    CANARD_ASSERT(memory_free != NULL);
    const CanardInstance out = {
        .user_reference    = NULL,
        .mtu_bytes         = CANARD_MTU_CAN_FD,
        .node_id           = CANARD_NODE_ID_UNSET,
        .memory_allocate   = memory_allocate,
        .memory_free       = memory_free,
        ._rx_subscriptions = {NULL},
        ._tx_queue         = NULL,
    };
    return out;
}

int32_t canardTxPush(CanardInstance* const ins, const CanardTransfer* const transfer)
{
    int32_t out = -CANARD_ERROR_INVALID_ARGUMENT;
    if ((ins != NULL) && (transfer != NULL) && ((transfer->payload != NULL) || (0U == transfer->payload_size)))
    {
        const size_t  pl_mtu       = getPresentationLayerMTU(ins);
        const int32_t maybe_can_id = makeCANID(transfer, ins->node_id, pl_mtu);
        if (maybe_can_id >= 0)
        {
            if (transfer->payload_size <= pl_mtu)
            {
                out = pushSingleFrameTransfer(ins,
                                              transfer->timestamp_usec,
                                              (uint32_t) maybe_can_id,
                                              transfer->transfer_id,
                                              transfer->payload_size,
                                              transfer->payload);
            }
            else
            {
                out = pushMultiFrameTransfer(ins,
                                             pl_mtu,
                                             transfer->timestamp_usec,
                                             (uint32_t) maybe_can_id,
                                             transfer->transfer_id,
                                             transfer->payload_size,
                                             transfer->payload);
            }
        }
        else
        {
            out = maybe_can_id;
        }
    }
    return out;
}

int8_t canardTxPeek(const CanardInstance* const ins, CanardFrame* const out_frame)
{
    int8_t out = -CANARD_ERROR_INVALID_ARGUMENT;
    if ((ins != NULL) && (out_frame != NULL))
    {
        const CanardInternalTxQueueItem* const tqi = ins->_tx_queue;
        if (tqi != NULL)
        {
            out_frame->timestamp_usec  = tqi->deadline_usec;
            out_frame->extended_can_id = tqi->id;
            out_frame->payload_size    = tqi->payload_size;
            out_frame->payload         = &tqi->payload[0];
            out                        = 1;
        }
        else
        {
            out = 0;
        }
    }
    return out;
}

void canardTxPop(CanardInstance* const ins)
{
    if ((ins != NULL) && (ins->_tx_queue != NULL))
    {
        CanardInternalTxQueueItem* const next = ins->_tx_queue->next;
        ins->memory_free(ins, ins->_tx_queue);
        ins->_tx_queue = next;
    }
}

int8_t canardRxAccept(CanardInstance* const    ins,
                      const CanardFrame* const frame,
                      const uint8_t            iface_index,
                      CanardTransfer* const    out_transfer)
{
    int8_t out = -CANARD_ERROR_INVALID_ARGUMENT;
    if ((ins != NULL) && (out_transfer != NULL) && (frame != NULL) && (frame->extended_can_id <= CAN_EXT_ID_MASK) &&
        ((frame->payload != NULL) || (0 == frame->payload_size)))
    {
        FrameModel model = {0};
        if (tryParseFrame(frame, &model))
        {
            if ((CANARD_NODE_ID_UNSET == model.destination_node_id) || (ins->node_id == model.destination_node_id))
            {
                out = acceptFrame(ins, &model, iface_index, out_transfer);
            }
            else
            {
                out = 0;  // Mis-addressed frame is obviously not an error.
            }
        }
        else
        {
            out = 0;  // A non-UAVCAN/CAN input frame is obviously not an error.
        }
    }
    return out;
}

int8_t canardRxSubscribe(CanardInstance* const       ins,
                         const CanardTransferKind    transfer_kind,
                         const CanardPortID          port_id,
                         const size_t                payload_size_bytes_max,
                         const CanardMicrosecond     transfer_id_timeout_usec,
                         CanardRxSubscription* const out_subscription)
{
    int8_t       out = -CANARD_ERROR_INVALID_ARGUMENT;
    const size_t tk  = (size_t) transfer_kind;
    if ((ins != NULL) && (out_subscription != NULL) && (tk < CANARD_NUM_TRANSFER_KINDS))
    {
        // Reset to the initial state. This is absolutely critical because the new payload size limit may be larger
        // than the old value; if there are any payload buffers allocated, we may overrun them because they are shorter
        // than the new payload limit. So we clear the subscription and thus ensure that no overrun may occur.
        out = canardRxUnsubscribe(ins, transfer_kind, port_id);
        if (out >= 0)
        {
            for (size_t i = 0; i < SESSIONS_PER_SUBSCRIPTION; i++)
            {
                // The sessions will be created ad-hoc. Normally, for a low-jitter deterministic system,
                // we could have pre-allocated sessions here, but that requires too much memory to be feasible.
                // We could accept an extra argument that would instruct us to pre-allocate sessions here?
                out_subscription->_sessions[i] = NULL;
            }
            out_subscription->_transfer_id_timeout_usec = transfer_id_timeout_usec;
            out_subscription->_payload_size_bytes_max   = payload_size_bytes_max;
            out_subscription->_port_id                  = port_id;
            out_subscription->_next                     = ins->_rx_subscriptions[tk];
            ins->_rx_subscriptions[tk]                  = out_subscription;
            out                                         = (out > 0) ? 0 : 1;
        }
    }
    return out;
}

int8_t canardRxUnsubscribe(CanardInstance* const    ins,
                           const CanardTransferKind transfer_kind,
                           const CanardPortID       port_id)
{
    int8_t       out = -CANARD_ERROR_INVALID_ARGUMENT;
    const size_t tk  = (size_t) transfer_kind;
    if ((ins != NULL) && (tk < CANARD_NUM_TRANSFER_KINDS))
    {
        CanardRxSubscription* prv = NULL;
        CanardRxSubscription* sub = ins->_rx_subscriptions[tk];
        while ((sub != NULL) && (sub->_port_id != port_id))
        {
            prv = sub;
            sub = sub->_next;
        }

        if (sub != NULL)
        {
            CANARD_ASSERT(sub->_port_id == port_id);
            out = 1;

            if (prv != NULL)
            {
                prv->_next = sub->_next;
            }
            else
            {
                ins->_rx_subscriptions[tk] = sub->_next;
            }

            for (size_t i = 0; i < SESSIONS_PER_SUBSCRIPTION; i++)
            {
                ins->memory_free(ins, (sub->_sessions[i] != NULL) ? sub->_sessions[i]->payload : NULL);
                ins->memory_free(ins, sub->_sessions[i]);
                sub->_sessions[i] = NULL;
            }
        }
        else
        {
            out = 0;
        }
    }
    return out;
}
