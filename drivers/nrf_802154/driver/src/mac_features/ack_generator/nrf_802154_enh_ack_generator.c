/*
 * Copyright (c) 2018 - 2021, Nordic Semiconductor ASA
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of Nordic Semiconductor ASA nor the names of its
 *    contributors may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY, AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 */

/**
 * @file
 *   This file implements an enhanced acknowledgement (Enh-Ack) generator for 802.15.4 radio driver.
 *
 */

#include "nrf_802154_enh_ack_generator.h"

#include <assert.h>
#include <string.h>

#include "mac_features/nrf_802154_frame_parser.h"
#include "mac_features/nrf_802154_ie_writer.h"
#include "mac_features/nrf_802154_security_pib.h"
#include "nrf_802154_ack_data.h"
#include "nrf_802154_encrypt.h"
#include "nrf_802154_const.h"
#include "nrf_802154_pib.h"
#include "nrf_802154_utils_byteorder.h"

#define ENH_ACK_MAX_SIZE MAX_PACKET_SIZE

static uint8_t m_ack_data[ENH_ACK_MAX_SIZE + PHR_SIZE];

static void ack_buffer_clear(nrf_802154_frame_parser_data_t * p_ack_data)
{
    memset(&m_ack_data[PHR_OFFSET], 0U, PHR_SIZE + FCF_SIZE);
    (void)nrf_802154_frame_parser_data_init(m_ack_data, 0U, PARSE_LEVEL_NONE, p_ack_data);
}

static uint8_t sequence_number_set(const nrf_802154_frame_parser_data_t * p_frame_data)
{
    const uint8_t * p_frame_dsn = nrf_802154_frame_parser_dsn_get(p_frame_data);

    if (p_frame_dsn != NULL)
    {
        m_ack_data[DSN_OFFSET] = *p_frame_dsn;

        return DSN_SIZE;
    }

    return 0U;
}

/***************************************************************************************************
 * @section Frame control field functions
 **************************************************************************************************/

static void fcf_frame_type_set(void)
{
    m_ack_data[FRAME_TYPE_OFFSET] |= FRAME_TYPE_ACK;
}

static void fcf_security_enabled_set(const nrf_802154_frame_parser_data_t * p_frame_data)
{
    if (nrf_802154_frame_parser_security_enabled_bit_is_set(p_frame_data))
    {
        m_ack_data[SECURITY_ENABLED_OFFSET] |= SECURITY_ENABLED_BIT;
    }
}

static void fcf_frame_pending_set(const nrf_802154_frame_parser_data_t * p_frame_data)
{
    if (nrf_802154_ack_data_pending_bit_should_be_set(p_frame_data))
    {
        m_ack_data[FRAME_PENDING_OFFSET] |= FRAME_PENDING_BIT;
    }
}

static void fcf_panid_compression_set(const nrf_802154_frame_parser_data_t * p_frame_data)
{
    if (nrf_802154_frame_parser_panid_compression_is_set(p_frame_data))
    {
        m_ack_data[PAN_ID_COMPR_OFFSET] |= PAN_ID_COMPR_MASK;
    }
}

static void fcf_sequence_number_suppression_set(const nrf_802154_frame_parser_data_t * p_frame_data)
{
    if (nrf_802154_frame_parser_dsn_suppress_bit_is_set(p_frame_data))
    {
        m_ack_data[DSN_SUPPRESS_OFFSET] |= DSN_SUPPRESS_BIT;
    }
}

static void fcf_ie_present_set(bool ie_present)
{
    if (ie_present)
    {
        m_ack_data[IE_PRESENT_OFFSET] |= IE_PRESENT_BIT;
    }
}

static void fcf_dst_addressing_mode_set(const nrf_802154_frame_parser_data_t * p_frame_data)
{
    if (nrf_802154_frame_parser_src_addr_is_extended(p_frame_data))
    {
        m_ack_data[DEST_ADDR_TYPE_OFFSET] |= DEST_ADDR_TYPE_EXTENDED;
    }
    else if (nrf_802154_frame_parser_src_addr_is_short(p_frame_data))
    {
        m_ack_data[DEST_ADDR_TYPE_OFFSET] |= DEST_ADDR_TYPE_SHORT;
    }
    else
    {
        m_ack_data[DEST_ADDR_TYPE_OFFSET] |= DEST_ADDR_TYPE_NONE;
    }
}

static void fcf_src_addressing_mode_set(void)
{
    m_ack_data[SRC_ADDR_TYPE_OFFSET] |= SRC_ADDR_TYPE_NONE;
}

static void fcf_frame_version_set(void)
{
    m_ack_data[FRAME_VERSION_OFFSET] |= FRAME_VERSION_2;
}

static uint8_t frame_control_set(const nrf_802154_frame_parser_data_t * p_frame_data,
                                 bool                                   ie_present)
{
    fcf_frame_type_set();
    fcf_security_enabled_set(p_frame_data);
    fcf_frame_pending_set(p_frame_data);
    fcf_panid_compression_set(p_frame_data);
    fcf_sequence_number_suppression_set(p_frame_data);
    fcf_ie_present_set(ie_present);
    fcf_dst_addressing_mode_set(p_frame_data);
    fcf_frame_version_set();
    fcf_src_addressing_mode_set();

    return FCF_SIZE;
}

/***************************************************************************************************
 * @section Addressing fields functions
 **************************************************************************************************/

static uint8_t destination_set(const nrf_802154_frame_parser_data_t * p_frame_data,
                               nrf_802154_frame_parser_data_t       * p_ack_data)
{
    uint8_t   bytes_written   = 0U;
    uint8_t * p_ack_dst_panid = (uint8_t *)nrf_802154_frame_parser_dst_panid_get(p_ack_data);
    uint8_t * p_ack_dst_addr  = (uint8_t *)nrf_802154_frame_parser_dst_addr_get(p_ack_data);

    const uint8_t * p_frame_src_panid = nrf_802154_frame_parser_src_panid_get(p_frame_data);
    const uint8_t * p_frame_dst_panid = nrf_802154_frame_parser_dst_panid_get(p_frame_data);
    const uint8_t * p_frame_src_addr  = nrf_802154_frame_parser_src_addr_get(p_frame_data);

    uint8_t src_addr_size = nrf_802154_frame_parser_src_addr_size_get(p_frame_data);

    // Fill the Ack destination PAN ID field.
    if (p_ack_dst_panid != NULL)
    {
        const uint8_t * p_dst_panid;

        if (p_frame_src_panid != NULL)
        {
            p_dst_panid = p_frame_src_panid;
        }
        else if (p_frame_dst_panid != NULL)
        {
            p_dst_panid = p_frame_dst_panid;
        }
        else
        {
            p_dst_panid = nrf_802154_pib_pan_id_get();
        }

        memcpy(p_ack_dst_panid, p_dst_panid, PAN_ID_SIZE);
        bytes_written += PAN_ID_SIZE;
    }

    // Fill the Ack destination address field.
    if ((p_ack_dst_addr != NULL) && (p_frame_src_addr != NULL))
    {
        assert(nrf_802154_frame_parser_dst_addr_is_extended(p_ack_data) ==
               nrf_802154_frame_parser_src_addr_is_extended(p_frame_data));

        memcpy(p_ack_dst_addr, p_frame_src_addr, src_addr_size);
        bytes_written += src_addr_size;
    }

    return bytes_written;
}

static void source_set(void)
{
    // Intentionally empty: source address type is None.
}

/***************************************************************************************************
 * @section Auxiliary security header functions
 **************************************************************************************************/

static uint8_t security_header_size(const nrf_802154_frame_parser_data_t * p_frame_data)
{
    uint8_t sec_ctrl_offset = nrf_802154_frame_parser_sec_ctrl_offset_get(p_frame_data);
    uint8_t aux_sec_hdr_end = nrf_802154_frame_parser_aux_sec_hdr_end_offset_get(p_frame_data);

    return aux_sec_hdr_end - sec_ctrl_offset;
}

static uint8_t key_id_size_get(uint8_t key_id_mode)
{
    switch (key_id_mode)
    {
        case KEY_ID_MODE_1:
            return KEY_ID_MODE_1_SIZE;

        case KEY_ID_MODE_2:
            return KEY_ID_MODE_2_SIZE;

        case KEY_ID_MODE_3:
            return KEY_ID_MODE_3_SIZE;

        default:
            return 0U;
    }
}

static uint8_t security_key_id_set(const nrf_802154_frame_parser_data_t * p_frame_data,
                                   nrf_802154_frame_parser_data_t       * p_ack_data)
{
    const uint8_t * p_frame_key_id = nrf_802154_frame_parser_key_id_get(p_frame_data);
    uint8_t       * p_ack_key_id   = (uint8_t *)nrf_802154_frame_parser_key_id_get(p_ack_data);
    uint8_t         key_id_size    = key_id_size_get(nrf_802154_frame_parser_sec_ctrl_key_id_mode_get(
                                                         p_ack_data));

    if ((p_ack_key_id != NULL) && (p_frame_key_id != NULL))
    {
        memcpy(p_ack_key_id, p_frame_key_id, key_id_size);
    }

    return key_id_size;
}

static bool frame_counter_set(nrf_802154_frame_parser_data_t * p_ack_data,
                              uint8_t                        * p_bytes_written)
{
    nrf_802154_key_id_t ack_key_id;
    uint32_t            new_fc_value;
    uint8_t           * p_frame_counter = (uint8_t *)nrf_802154_frame_parser_frame_counter_get(
        p_ack_data);

    if (p_frame_counter == NULL)
    {
        // The frame counter is suppressed
        *p_bytes_written = 0;
        return true;
    }

    ack_key_id.mode     = nrf_802154_frame_parser_sec_ctrl_key_id_mode_get(p_ack_data);
    ack_key_id.p_key_id = (uint8_t *)nrf_802154_frame_parser_key_id_get(p_ack_data);

    if (NRF_802154_SECURITY_ERROR_NONE !=
        nrf_802154_security_pib_frame_counter_get_next(&new_fc_value, &ack_key_id))
    {
        *p_bytes_written = 0;
        return false;
    }

    // Set the frame counter value in security header of the ACK frame
    host_32_to_little(new_fc_value, p_frame_counter);
    *p_bytes_written = FRAME_COUNTER_SIZE;

    return true;
}

static bool security_header_set(const nrf_802154_frame_parser_data_t * p_frame_data,
                                nrf_802154_frame_parser_data_t       * p_ack_data,
                                uint8_t                              * p_bytes_written)
{
    bool security_header_prepared;
    bool result;

    uint8_t bytes_written       = 0U;
    uint8_t fc_bytes_written    = 0U;
    uint8_t ack_sec_ctrl_offset = nrf_802154_frame_parser_addressing_end_offset_get(
        p_ack_data);
    uint8_t * ack_sec_ctrl = (uint8_t *)nrf_802154_frame_parser_addressing_end_get(
        p_ack_data);
    const uint8_t * frame_sec_ctrl = nrf_802154_frame_parser_sec_ctrl_get(p_frame_data);

    if ((ack_sec_ctrl == NULL) || (frame_sec_ctrl == NULL))
    {
        *p_bytes_written = bytes_written;
        return true;
    }

    // All the bits in the security control byte can be copied.
    *ack_sec_ctrl  = *frame_sec_ctrl;
    bytes_written += SECURITY_CONTROL_SIZE;

    // Security control field is now ready. The parsing of the frame can advance.
    result = nrf_802154_frame_parser_valid_data_extend(p_ack_data,
                                                       ack_sec_ctrl_offset + PHR_SIZE,
                                                       PARSE_LEVEL_SEC_CTRL_OFFSETS);
    assert(result);
    (void)result;

    if (nrf_802154_frame_parser_sec_ctrl_sec_lvl_get(p_frame_data) == SECURITY_LEVEL_NONE)
    {
        // The security level value is zero, therefore no auxiliary security header processing
        // is performed according to 802.15.4 specification. This also applies to the frame counter,
        // the value of which is left as it is in the message to which the ACK responds.
        // The entire auxiliary security header content is simply copied to ACK.
        uint8_t sec_hdr_size = security_header_size(p_frame_data) - SECURITY_CONTROL_SIZE;

        memcpy(ack_sec_ctrl + SECURITY_CONTROL_SIZE,
               frame_sec_ctrl + SECURITY_CONTROL_SIZE,
               sec_hdr_size);
        bytes_written           += sec_hdr_size;
        security_header_prepared = true;
    }
    else
    {
        bytes_written           += security_key_id_set(p_frame_data, p_ack_data);
        security_header_prepared = frame_counter_set(p_ack_data, &fc_bytes_written);
        bytes_written           += fc_bytes_written;
    }

    bytes_written   += nrf_802154_frame_parser_mic_size_get(p_ack_data);
    *p_bytes_written = bytes_written;

    return security_header_prepared;
}

/***************************************************************************************************
 * @section Information Elements
 **************************************************************************************************/

static void ie_header_set(const uint8_t                  * p_ie_data,
                          uint8_t                          ie_data_len,
                          nrf_802154_frame_parser_data_t * p_ack_data)
{
    uint8_t   ie_offset = p_ack_data->helper.aux_sec_hdr_end_offset;
    uint8_t * p_ack_ie;

    p_ack_ie = (uint8_t *)p_ack_data->p_frame + ie_offset;

    if (p_ie_data == NULL)
    {
        return;
    }

    assert(p_ack_ie != NULL);

    memcpy(p_ack_ie, p_ie_data, ie_data_len);

#if NRF_802154_IE_WRITER_ENABLED
    nrf_802154_ie_writer_prepare(p_ack_ie, p_ack_ie + ie_data_len);
#endif
}

static uint8_t ie_header_terminate(const uint8_t                  * p_ie_data,
                                   uint8_t                          ie_data_len,
                                   nrf_802154_frame_parser_data_t * p_ack_data)
{
    if (p_ie_data == NULL)
    {
        // No IEs to terminate.
        return 0U;
    }

    if ((nrf_802154_frame_parser_security_enabled_bit_is_set(p_ack_data) == false) ||
        (nrf_802154_frame_parser_sec_ctrl_sec_lvl_get(p_ack_data) == SECURITY_LEVEL_NONE))
    {
        // This code assumes that neither regular frame payload nor Payload IEs can be set by the
        // driver. Therefore without security, the Ack has no payload, so termination is not necessary.
        return 0U;
    }

    uint8_t * p_ack_ie = (uint8_t *)p_ack_data->p_frame + p_ack_data->helper.aux_sec_hdr_end_offset;
    uint8_t   ie_hdr_term[IE_HEADER_SIZE];

    assert(p_ack_ie != NULL);

    host_16_to_little((IE_HT2) << IE_HEADER_ELEMENT_ID_OFFSET, ie_hdr_term);

    memcpy(p_ack_ie + ie_data_len, ie_hdr_term, sizeof(ie_hdr_term));
    return sizeof(ie_hdr_term);
}

/***************************************************************************************************
 * @section Authentication and encryption transformation
 **************************************************************************************************/

static bool encryption_prepare(const nrf_802154_frame_parser_data_t * p_ack_data)
{
#if NRF_802154_ENCRYPTION_ENABLED
    if (nrf_802154_frame_parser_security_enabled_bit_is_set(p_ack_data) == false)
    {
        return true;
    }

    if (nrf_802154_frame_parser_sec_ctrl_sec_lvl_get(p_ack_data) == SECURITY_LEVEL_NONE)
    {
        return true;
    }

    return nrf_802154_encrypt_ack_prepare(p_ack_data);
#else // NRF_802154_ENCRYPTION_ENABLED
    return true;
#endif  // NRF_802154_ENCRYPTION_ENABLED
}

/***************************************************************************************************
 * @section Public API implementation
 **************************************************************************************************/

void nrf_802154_enh_ack_generator_init(void)
{
    // Intentionally empty.
}

const uint8_t * nrf_802154_enh_ack_generator_create(
    const nrf_802154_frame_parser_data_t * p_frame_data)
{
    nrf_802154_frame_parser_data_t ack_data;

    bool    result;
    uint8_t bytes_written = 0U;
    uint8_t ie_data_len   = 0U;

    // coverity[unchecked_value]
    const uint8_t * p_ie_data = nrf_802154_ack_data_ie_get(
        nrf_802154_frame_parser_src_addr_get(p_frame_data),
        nrf_802154_frame_parser_src_addr_is_extended(p_frame_data),
        &ie_data_len);

    // Clear previously created ACK.
    ack_buffer_clear(&ack_data);

    // Set Frame Control field bits.
    bytes_written           = frame_control_set(p_frame_data, p_ie_data != NULL);
    m_ack_data[PHR_OFFSET] += bytes_written;

    result = nrf_802154_frame_parser_valid_data_extend(&ack_data,
                                                       m_ack_data[PHR_OFFSET] + PHR_SIZE,
                                                       PARSE_LEVEL_FCF_OFFSETS);
    assert(result);
    (void)result;

    // Set valid sequence number in ACK frame.
    bytes_written           = sequence_number_set(p_frame_data);
    m_ack_data[PHR_OFFSET] += bytes_written;

    // Set destination address and PAN ID.
    bytes_written           = destination_set(p_frame_data, &ack_data);
    m_ack_data[PHR_OFFSET] += bytes_written;

    // Set source address and PAN ID.
    source_set();

    if (security_header_set(p_frame_data, &ack_data, &bytes_written) == false)
    {
        // Failure to set auxiliary security header: The ACK cannot be created.
        ack_buffer_clear(&ack_data);
        return NULL;
    }

    m_ack_data[PHR_OFFSET] += bytes_written;

    result = nrf_802154_frame_parser_valid_data_extend(&ack_data,
                                                       m_ack_data[PHR_OFFSET] + PHR_SIZE,
                                                       PARSE_LEVEL_AUX_SEC_HDR_END);
    assert(result);
    (void)result;

    // Set IE header.
    ie_header_set(p_ie_data, ie_data_len, &ack_data);
    m_ack_data[PHR_OFFSET] += ie_data_len;

    // Terminate the IE header if needed.
    bytes_written           = ie_header_terminate(p_ie_data, ie_data_len, &ack_data);
    m_ack_data[PHR_OFFSET] += bytes_written + FCS_SIZE;

    result = nrf_802154_frame_parser_valid_data_extend(&ack_data,
                                                       m_ack_data[PHR_OFFSET] + PHR_SIZE,
                                                       PARSE_LEVEL_FULL);
    assert(result);
    (void)result;

    // Prepare encryption.
    if (!encryption_prepare(&ack_data))
    {
        // Failure to prepare encryption even though it's required. The ACK cannot be created.
        ack_buffer_clear(&ack_data);
        return NULL;
    }

    return m_ack_data;
}

#ifdef TEST
void nrf_802154_enh_ack_generator_module_reset(void)
{
    memset(m_ack_data, 0U, sizeof(m_ack_data));
}

#endif // TEST
