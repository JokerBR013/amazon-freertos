/*
 * Copyright (C) 2020 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include <string.h>

#include "FreeRTOS.h"
#include "stream_buffer.h"
#include "iot_ble.h"
#include "iot_ble_mqtt_transport.h"
#include "iot_ble_mqtt_serialize.h"
#include "core_mqtt_serializer.h"

/*-----------------------------------------------------------*/

/* Size of CONNACK, PUBACK. */
#define SIZE_OF_SIMPLE_ACK    4U

/* Size of DISCONNECT, PINGRESP and PINGREQ. */
#define SIZE_OF_PING          2U

/* Size of SUBACK, UNSUBACK.
 * As of now, the only one ACK is received for any request
 * and the size is always 5.
 */
#define SIZE_OF_SUB_ACK       5U

#define MAX_ENCODE_LENGTH     4

#define BIT_MASK( bitnum )    ( ( uint8_t ) ( 1 << bitnum ) )

/* Define masks for each flag in Connect packets */
#define CLEAN_SESSION_MASK    0x02U

#define WILL_FLAG_MASK        0X04U

#define WILL_QOS_MASK         0x18U

#define WILL_RETAIN_MASK      0x20U

#define PASSWORD_MASK         0x40U

#define USERNAME_MASK         0x80U

#define CLIENT_PACKET_TYPE_CONNECT        ( ( uint8_t ) 0x10U )  /**< @brief CONNECT (client-to-server). */
#define CLIENT_PACKET_TYPE_PUBLISH        ( ( uint8_t ) 0x30U )  /**< @brief PUBLISH (bidirectional). */
#define CLIENT_PACKET_TYPE_PUBACK         ( ( uint8_t ) 0x40U )  /**< @brief PUBACK (bidirectional). */
#define CLIENT_PACKET_TYPE_PUBREC         ( ( uint8_t ) 0x50U )  /**< @brief PUBREC (bidirectional). */
#define CLIENT_PACKET_TYPE_PUBREL         ( ( uint8_t ) 0x60U )  /**< @brief PUBREL (bidirectional). */
#define CLIENT_PACKET_TYPE_PUBCOMP        ( ( uint8_t ) 0x70U )  /**< @brief PUBCOMP (bidirectional). */
#define CLIENT_PACKET_TYPE_SUBSCRIBE      ( ( uint8_t ) 0x80U )  /**< @brief SUBSCRIBE (client-to-server). */
#define CLIENT_PACKET_TYPE_UNSUBSCRIBE    ( ( uint8_t ) 0xA0U )  /**< @brief UNSUBSCRIBE (client-to-server). */
#define CLIENT_PACKET_TYPE_PINGREQ        ( ( uint8_t ) 0xC0U )  /**< @brief PINGREQ (client-to-server). */
#define CLIENT_PACKET_TYPE_DISCONNECT     ( ( uint8_t ) 0xE0U )  /**< @brief DISCONNECT (client-to-server). */

/* Define masks for bit postions of each flag in Publish packet flag. */
#define PUBLISH_FLAG_RETAIN_MASK                    BIT_MASK( 0 )                     /**< @brief MQTT PUBLISH retain flag. */
#define PUBLISH_FLAG_QOS_MASK                       ( BIT_MASK( 1 ) | BIT_MASK( 2 ) ) /**< @brief MQTT PUBLISH QoS1 flag. */
#define PUBLISH_FLAG_DUP_MASK                       BIT_MASK( 2 )                     /**< @brief MQTT PUBLISH duplicate flag. */
#define PUBLISH_FLAG_QOS1_MASK                      BIT_MASK( 1 ) /**< @brief MQTT PUBLISH QoS1 flag. */
#define PUBLISH_FLAG_QOS2_MASK                      BIT_MASK( 2 ) /**< @brief MQTT PUBLISH QoS2 flag. */

#define REMAINING_LENGTH_INVALID            ( ( size_t ) 268435456 )

/**
 * @brief Macro for checking if a bit is set in a 1-byte unsigned int.
 *
 * @param[in] x The unsigned int to check.
 * @param[in] position Which bit to check.
 */
#define UINT8_CHECK_BIT( x, position )    ( ( ( x ) & ( 0x01U << ( position ) ) ) == ( 0x01U << ( position ) ) )

 /* @brief Macro for decoding a 2-byte unsigned int from a sequence of bytes.
 *
 * @param[in] ptr A uint8_t* that points to the high byte.
 */
#define UINT16_DECODE( ptr )                                \
    ( uint16_t ) ( ( ( ( uint16_t ) ( *( ptr ) ) ) << 8 ) | \
                   ( ( uint16_t ) ( *( ( ptr ) + 1 ) ) ) )

/*-----------------------------------------------------------*/

/**
 * @brief Returns the integer represented by two length bytes.
 *
 * @param[in] buf A pointer to the MSB of the integer.
 */
static uint16_t getIntFromTwoBytes( const uint8_t * buf );


/**
 * @brief Converts the given integer to MQTTQoS_t
 *
 * @param[in] incomingQos, the integer to convert
 */
static MQTTQoS_t convertIntToQos( const uint8_t incomingQos );

/**
 * @brief Sets flags in connectConfig according to the packet in buf.
 *
 * @param[in] connectConfig, expected to be empty.
 * @param[in] buf, which points to the MQTT packet.
 * @return MQTTSuccess or MQTTBadParameter
 */
static MQTTStatus_t parseConnect( MQTTConnectInfo_t * connectConfig,
                                  const uint8_t * buf );

/**
 * @brief Calculates number of subscriptions requested in a subscribe packet.
 *
 * @param[in] buf, whic points to the MQTT packet.
 * @param[in] remainingLength which is the size of the rest of the packet.
 * @param[in] subscribe, true iff you are parsing a subscribe packet, false if unsubscribe.
 * @return the number of subscriptions.
 */
static uint16_t calculateNumSubs( const uint8_t * buf,
                                  uint8_t remainingLength,
                                  bool subscribe );

/**
 * @brief Sets flags in global var subscriptions according to the packet in buf.
 *
 * @param[in] subscription count, a pointer to a size_t with num subscriptions.
 * @param[in] buf, which points to the MQTT packet.
 * @param[out] pIdentifier, which points to packet identifier.
 * @param[in] subscribe, true iff you are parsing a subscribe packet, false if unsubscribe.
 * @return MQTTSuccess or MQTTBadParameter
 */
static MQTTStatus_t parseSubscribe( size_t * subscriptionCount,
                                    const uint8_t * buf,
                                    uint16_t * pIdentifier,
                                    bool subscribe );

/**
 * @brief Serializes an ACK packet in MQTT format.
 *
 * @param[in] pBuffer, which is the buffer to write the packet to.
 * @param[in] packetType which corresponds to which ack should be written.
 * @param[in] packetId, the packet Identifier (unused with CONNACK packets).
 * @return returns MQTTSuccess, MQTTBadParameter, or MQTTNoMemory.
 */
static MQTTStatus_t transportSerializeSuback( MQTTFixedBuffer_t * pBuffer,
                                              uint8_t packetType,
                                              uint16_t packetId );


/**
 * @brief Serializes a SUBACK packet in MQTT format.
 *
 * @param[in] pBuffer, which is the buffer to write the packet to.
 * @param[in] packetType which corresponds to which ack should be written.
 * @param[in] packetId, the packet Identifier.
 * @return returns MQTTSuccess, MQTTBadParameter, or MQTTNoMemory.
 */
static MQTTStatus_t transportSerializeAck( MQTTFixedBuffer_t * pBuffer,
                                           uint8_t packetType,
                                           uint16_t packetId );


/**
 * @brief Serializes a Pingresp packet in MQTT Format.
 *
 * @param[in] pBuffer, which is the buffer to write the packet to.
 * @return returns MQTTSuccess, MQTTBadParameter, or MQTTNoMemory.
 */
static MQTTStatus_t transportSerializePingresp( MQTTFixedBuffer_t * pBuffer );


/**
 * @brief Deserializes a Publish packet in MQTT Format
 * @param[in]
 *
 * statis MQTTStatus_t */
/*-----------------------------------------------------------*/

/**
 * @brief Holds the list of topic filters to put into the subscribe packet.
 */
static MQTTSubscribeInfo_t _subscriptions[ MQTT_MAX_SUBS_PER_PACKET ];

/**
 * @brief Data structure to hold received data before user makes a request for it.
 */

/*-----------------------------------------------------------*/


bool IotBleMqttTransportInit( NetworkContext_t * pContext )
{
    bool status = true;

    pContext->xStreamBuffer = xStreamBufferCreateStatic( pContext->bufSize, 1, pContext->buf, & ( pContext->xStreamBufferStruct ) );

    if( pContext->xStreamBuffer == NULL )
    {
        LogError( ( "BLE teransport layer buffer could not be created.  Check the buffer and buffer size passed to network context." ) );
        status = false;
    }

    return status;
}


void IotBleMqttTransportCleanup( NetworkContext_t * pContext )
{
    vStreamBufferDelete( pContext->xStreamBuffer );
}


static uint16_t getIntFromTwoBytes( const uint8_t * buf )
{
    uint16_t result = 0;

    result = buf[ 0 ];
    result <<= 8;
    result += buf[ 1 ];
    return result;
}


static MQTTQoS_t convertIntToQos( const uint8_t incomingQos )
{
    MQTTQoS_t qosValue = MQTTQoS0;

    switch( incomingQos )
    {
        case 0U:
            qosValue = MQTTQoS0;
            break;

        case 1U:
            qosValue = MQTTQoS1;
            break;

        default:
            LogError( ( "QoS 2 is not supported by MQTT over BLE. Defaulting to Qos 1." ) );
            qosValue = MQTTQoS1;
            break;
    }

    return qosValue;
}


static MQTTStatus_t parseConnect( MQTTConnectInfo_t * connectConfig,
                                  const uint8_t * buf )
{
    bool willFlag = false;
    bool willRetain = false;
    bool passwordFlag = false;
    bool usernameFlag = false;
    MQTTStatus_t status = MQTTSuccess;
    MQTTQoS_t willQos = 0;
    uint8_t connectionFlags = 0;
    size_t bufferIndex = 0;


    configASSERT(( ( buf[ 0 ] & 0xf0U ) == MQTT_PACKET_TYPE_CONNECT ));

    if( status == MQTTSuccess )
    {
        /* Skips the remaining length, which is not needed in connect packet.  Finds first 'M' (77) in 'MQTT'. */
        while( buf[ bufferIndex ] != 77U )
        {
            bufferIndex++;
        }

        /* Skips 'MQTT'. */
        bufferIndex += 4U;

        /* The service level of the packet. Must be 4. */
        if( ( buf[ bufferIndex ] != 4U ) )
        {
            LogError( ( "The service level of a connect packet must be 4, see [MQTT-3.1.2-2]." ) );
            status = MQTTBadParameter;
        }

        /* Increment bufferIndex to be the start of the payload. */
        bufferIndex++;
        /* Second byte after payload is connection flags. */
        connectionFlags = buf[ bufferIndex ];

        /* The LSB is reserved and must be 0. */
        if( ( connectionFlags & 0x01U ) != 0U )
        {
            LogError( ( "LSB of Connect Flags byte must be 0, see [MQTT-3.1.2-3]." ) );
            status = MQTTBadParameter;
        }
    }

    if( status == MQTTSuccess )
    {
        /* Get flag data from the flag byte. */
        connectConfig->cleanSession = ( ( connectionFlags & CLEAN_SESSION_MASK ) == CLEAN_SESSION_MASK );
        willFlag = ( ( connectionFlags & WILL_FLAG_MASK ) == WILL_FLAG_MASK );
        willQos = convertIntToQos( ( connectionFlags & WILL_QOS_MASK ) >> 3 );
        willRetain =   ( ( connectionFlags & WILL_RETAIN_MASK ) == WILL_RETAIN_MASK );
        passwordFlag = ( ( connectionFlags & PASSWORD_MASK ) == PASSWORD_MASK );
        usernameFlag = ( ( connectionFlags & USERNAME_MASK ) == USERNAME_MASK );

        /* The will retain flag is currently unused */
        ( void ) willRetain;
    }

    if( status == MQTTSuccess )
    {
        /* Third and fourth bytes are keep alive time */
        connectConfig->keepAliveSeconds = getIntFromTwoBytes( &buf[ bufferIndex + 1U ] );

        /* Start of the payload */
        bufferIndex += 3U;

        /* Client identifier is required, 2 length bytes and then identifier */
        connectConfig->clientIdentifierLength = getIntFromTwoBytes( &buf[ bufferIndex ] );
        connectConfig->pClientIdentifier = ( const char * ) &buf[ bufferIndex + 2U ];
    }

    if( ( status == MQTTSuccess ) && ( connectConfig->clientIdentifierLength == 0U ) )
    {
        LogError( ( "A client identifier must be present in a connect packet [MQTT-3.1.3-3]." ) );
        status = MQTTBadParameter;
    }

    /* Set to start of rest of payload. */
    bufferIndex = bufferIndex + 2U + connectConfig->clientIdentifierLength;

    if( ( status == MQTTSuccess ) && ( willFlag == true ) )
    {
        /* Populate Last Will header information. */
        MQTTPublishInfo_t willInfo;
        willInfo.qos = willQos;
        willInfo.retain = willRetain;
        willInfo.topicNameLength = getIntFromTwoBytes( &buf[ bufferIndex ] );
        willInfo.pTopicName = ( const char * ) &buf[ bufferIndex + 2U ];

        if( willInfo.topicNameLength == 0U )
        {
            LogError( ( "The will flag was set but no will topic was given." ) );
            status = MQTTBadParameter;
        }
        else
        {
            bufferIndex = bufferIndex + 2U + willInfo.topicNameLength;

            /* Populate Last Will payload information. */
            willInfo.payloadLength = getIntFromTwoBytes( &buf[ bufferIndex ] );
            willInfo.pPayload = ( const char * ) &buf[ bufferIndex + 2U ];

            bufferIndex = bufferIndex + 2U + willInfo.payloadLength;
        }
    }

    if( ( status == MQTTSuccess ) && ( usernameFlag == true ) )
    {
        /* Populate Username header information. */
        connectConfig->userNameLength = getIntFromTwoBytes( &buf[ bufferIndex ] );
        connectConfig->pUserName = ( const char * ) &buf[ bufferIndex + 2U ];

        if( connectConfig->userNameLength == 0U )
        {
            LogError( ( "The username flag was set but no username was given." ) );
            status = MQTTBadParameter;
        }

        bufferIndex = bufferIndex + 2U + connectConfig->userNameLength;
    }

    if( ( status == MQTTSuccess ) && ( passwordFlag == true ) )
    {
        /* Populate Password header information. */
        connectConfig->passwordLength = getIntFromTwoBytes( &buf[ bufferIndex ] );
        connectConfig->pPassword = ( const char * ) &buf[ bufferIndex + 2U ];

        if( connectConfig->passwordLength == 0U )
        {
            LogError( ( "The password flag was set but no password was given." ) );
            status = MQTTBadParameter;
        }

        bufferIndex = bufferIndex + 2U + connectConfig->passwordLength;
    }

    return status;
}

static size_t getRemainingLength( const uint8_t * buf, size_t *pEncodedLength )
{
    size_t remainingLength = 0, multiplier = 1;
    uint8_t encodedByte = 0;
    size_t encodedLength = 0;

    /* This algorithm is adapted from the MQTT v3.1.1 spec. */
    do
    {
        if( multiplier > 2097152U ) /* 128 ^ 3 */
        {
            remainingLength = REMAINING_LENGTH_INVALID;
            break;
        }
        else
        {
            encodedByte = buf[ encodedLength ];
            remainingLength += ( ( size_t ) encodedByte & 0x7FU ) * multiplier;
            multiplier *= 128U;
            encodedLength++;
        }
    } while( ( encodedByte & 0x80U ) != 0U );


    *pEncodedLength = encodedLength;

    return remainingLength;
}


static bool parsePublish( const uint8_t * buf,
                          size_t length,
                          MQTTBLEPublishInfo_t * pPublishInfo )
{
    uint8_t publishFlags = 0;
    size_t encodedLength = 0, remainingLength = length;
    size_t index = 0;
    bool ret = false;
    void *topicBuffer = NULL;

    /* Parse the publish header. */
    configASSERT(( ( buf[ index ] & 0xF0U ) == CLIENT_PACKET_TYPE_PUBLISH ));
    publishFlags = ( buf[ index ] & 0x0FU );

    pPublishInfo->info.dup = ( ( publishFlags & PUBLISH_FLAG_DUP_MASK ) == PUBLISH_FLAG_DUP_MASK );
    pPublishInfo->info.retain = ( ( publishFlags & PUBLISH_FLAG_RETAIN_MASK ) == PUBLISH_FLAG_RETAIN_MASK );
    pPublishInfo->info.qos = convertIntToQos( ( publishFlags & PUBLISH_FLAG_QOS_MASK ) >> 1 );    
    index++;
    
    remainingLength = getRemainingLength( &buf[ index ], &encodedLength );
    configASSERT(( remainingLength != REMAINING_LENGTH_INVALID ));
    index += encodedLength;

    pPublishInfo->info.topicNameLength = UINT16_DECODE( &buf[ index ] );
    index += sizeof( uint16_t );
    
    topicBuffer = IotMqtt_MallocMessage( pPublishInfo->info.topicNameLength );
    if( topicBuffer == NULL )
    {
        LogError(( "Failed to allocate buffer for topic name." ));
    }
    else
    {
        memcpy( topicBuffer, &buf[ index ], pPublishInfo->info.topicNameLength );
        pPublishInfo->info.pTopicName = topicBuffer;
    }
    
    index += pPublishInfo->info.topicNameLength;
    if( pPublishInfo->info.qos > MQTTQoS0 )
    {
        pPublishInfo->packetIdentifier = UINT16_DECODE( &buf[ index ] );
        index += sizeof( uint16_t );
    }

    pPublishInfo->info.payloadLength = ( remainingLength + 1U + encodedLength - index );

    if( index < length )
    {
        pPublishInfo->info.pPayload = &buf[ index ];
    }
    else
    {
        if( pPublishInfo->info.payloadLength > 0 )
        {
            ret = true;
        }
    }

    return ret;
}

static uint16_t calculateNumSubs( const uint8_t * buf,
                                  uint8_t remainingLength,
                                  bool subscribe )
{
    uint16_t numSubs = 0, totalSize = 0;

    /* Loop through the rest of the packet. */
    while( ( remainingLength - totalSize ) > 0 )
    {
        totalSize += ( getIntFromTwoBytes( &buf[ totalSize ] ) + 2U );

        if( subscribe )
        {
            /* Increment by 1 for Qos Byte. If it is unsubscribe packet, skip this. */
            totalSize++;
        }

        numSubs++;
    }

    return numSubs;
}

static MQTTStatus_t parseSubscribe( size_t * subscriptionCount,
                                    const uint8_t * buf,
                                    uint16_t * pIdentifier,
                                    bool subscribe )
{
    MQTTStatus_t status = MQTTSuccess;
    uint8_t remainingLength = 0;
    uint16_t bufferIndex = 0;
    size_t subscriptionIndex = 0;

    *pIdentifier = getIntFromTwoBytes( &buf[ 2 ] );

    /* Stores the remaining length after the identifier */
    remainingLength = buf[ 1 ] - 2U;

    *subscriptionCount = calculateNumSubs( &buf[ 4 ], remainingLength, subscribe );

    if( *subscriptionCount == 0U )
    {
        LogError( ( "Topic filters must exist in a subscribe packet.  See [MQTT-3.8.3-3]." ) );
        status = MQTTBadParameter;
    }

    if( status == MQTTSuccess )
    {
        bufferIndex = 4U;

        for( subscriptionIndex = 0; subscriptionIndex < *subscriptionCount; ++subscriptionIndex )
        {
            /* Populate the name of the topic to (un)subscribe to. */
            _subscriptions[ subscriptionIndex ].topicFilterLength = getIntFromTwoBytes( &buf[ bufferIndex ] );
            _subscriptions[ subscriptionIndex ].pTopicFilter = ( const char * ) &buf[ bufferIndex + 2U ];
            bufferIndex += _subscriptions[ subscriptionIndex ].topicFilterLength + 2U;

            /* For subscribe packets only, increment past qos byte. */
            if( subscribe == true )
            {
                _subscriptions[ subscriptionIndex ].qos = convertIntToQos( buf[ bufferIndex ] & 0x03U );
                bufferIndex++;
            }
        }
    }

    return status;
}


static uint16_t encodeRemainingLength( uint8_t * pBuffer,
                                        size_t length )
{
    uint16_t encodeLength = 0;
    uint8_t encodeByte;

    /* This algorithm is copied from the MQTT v3.1.1 spec. */
    do
    {
        encodeByte = ( uint8_t ) ( length % 128U );
        length = length / 128U;

        /* Set the high bit of this byte, indicating that there's more data. */
        if( length > 0U )
        {
            encodeByte |= BIT_MASK( 7U );
        }
        /* Output a single encoded byte. */
        *pBuffer = encodeByte;
        pBuffer++;
        encodeLength++;
    } while( length > 0U );

    return encodeLength;
}

static MQTTStatus_t transportSerializePublish( StreamBufferHandle_t streamBuffer,
                                               MQTTPublishInfo_t * pPublishConfig,
                                               uint16_t packetId )
{
    MQTTStatus_t status = MQTTSuccess;
    uint8_t header[ 5 ];
    uint8_t serializedArray[ 2 ];
    uint8_t publishFlags = MQTT_PACKET_TYPE_PUBLISH;
    size_t remainingLength, encodedLength;

    if( pPublishConfig->qos == MQTTQoS2 )
    {
        /* BLE does not support QOS2 publishes. */
        status = MQTTBadParameter;
    }
    else if( pPublishConfig->qos == MQTTQoS1 && packetId == 0 )
    {
        status = MQTTBadParameter;
    }
    else
    {   
        if( pPublishConfig->dup == true )
        {
            publishFlags |= PUBLISH_FLAG_DUP_MASK;
        }
        if( pPublishConfig->retain == true )
        {
            publishFlags |= PUBLISH_FLAG_RETAIN_MASK;
        }
        if( pPublishConfig->qos == MQTTQoS1 ) {
            publishFlags |= PUBLISH_FLAG_QOS1_MASK;
        }
        if( pPublishConfig->qos == MQTTQoS2 ) {
            publishFlags |= PUBLISH_FLAG_QOS2_MASK;
        }
        header[0] = publishFlags;
        
        remainingLength = 2U + pPublishConfig->topicNameLength + pPublishConfig->payloadLength;
        if( pPublishConfig->qos > MQTTQoS0 )
        {
            remainingLength += 2U;
        }

        encodedLength = encodeRemainingLength( &header[1], remainingLength );
    }

    if( status == MQTTSuccess )
    {
        /* Send the Fixed header + encoded length into the buffer */
        ( void ) xStreamBufferSend( streamBuffer, ( void * ) header, encodedLength + 1, pdMS_TO_TICKS( RECV_TIMEOUT_MS ) );

        /* Send the topic length to the buffer */
        serializedArray[ 0 ] = ( uint8_t ) ( ( pPublishConfig->topicNameLength & 0xFF00U ) >> 8 );
        serializedArray[ 1 ] = ( uint8_t ) ( pPublishConfig->topicNameLength & 0x00FFU );
        ( void ) xStreamBufferSend( streamBuffer, ( void * ) serializedArray, 2U, pdMS_TO_TICKS( RECV_TIMEOUT_MS ) );
        
        /* Send the topic into the buffer */
        ( void ) xStreamBufferSend( streamBuffer, ( void * ) pPublishConfig->pTopicName, pPublishConfig->topicNameLength, pdMS_TO_TICKS( RECV_TIMEOUT_MS ) );

        if( pPublishConfig->qos >= MQTTQoS1 )
        {
            /* Send the packet identifier to the buffer; need to represent as two seperate bytes to resolve endianness. */
            serializedArray[ 0 ] = ( uint8_t ) ( ( packetId & 0xFF00U ) >> 8 );
            serializedArray[ 1 ] = ( uint8_t ) ( packetId & 0x00FFU );
            ( void ) xStreamBufferSend( streamBuffer, ( void * ) serializedArray, 2U, pdMS_TO_TICKS( RECV_TIMEOUT_MS ) );
        }

        /* Send the payload itself into the buffer */
        ( void ) xStreamBufferSend( streamBuffer, ( void * ) pPublishConfig->pPayload, pPublishConfig->payloadLength, pdMS_TO_TICKS( RECV_TIMEOUT_MS ) );
    }

    return status;
}

static MQTTStatus_t transportSerializePingresp( MQTTFixedBuffer_t * pBuffer )
{
    MQTTStatus_t status = MQTTSuccess;

    configASSERT(( pBuffer != NULL ));
    configASSERT(( pBuffer->size >= SIZE_OF_PING ));

    pBuffer->pBuffer[ 0 ] = MQTT_PACKET_TYPE_PINGRESP;
    pBuffer->pBuffer[ 1 ] = 0;

    return status;
}

static MQTTStatus_t transportSerializeAck( MQTTFixedBuffer_t * pBuffer,
                                           uint8_t packetType,
                                           uint16_t packetId )
{
    MQTTStatus_t status = MQTTSuccess;

    configASSERT(( pBuffer != NULL ));
    configASSERT(( pBuffer->size >= SIZE_OF_SIMPLE_ACK ));

    if( ( packetId == 0U ) && ( packetType == MQTT_PACKET_TYPE_PUBACK ) )
    {
        LogError( ( "Packet ID cannot be 0." ) );
        status = MQTTBadParameter;
    }
    else
    {
        pBuffer->pBuffer[ 0 ] = packetType;
        pBuffer->pBuffer[ 1 ] = 2;
        pBuffer->pBuffer[ 2 ] = ( uint8_t ) ( packetId >> 8 );
        pBuffer->pBuffer[ 3 ] = ( uint8_t ) ( packetId & 0x00ffU );
    }

    return status;
}


static MQTTStatus_t transportSerializeSuback( MQTTFixedBuffer_t * pBuffer,
                                              uint8_t packetType,
                                              uint16_t packetId )
{
    MQTTStatus_t status = MQTTSuccess;

    configASSERT(( pBuffer != NULL ));
    configASSERT(( pBuffer->size >= SIZE_OF_SUB_ACK ));

    if( packetId == 0U )
    {
        LogError( ( "Packet ID cannot be 0." ) );
        status = MQTTBadParameter;
    }
    else
    {
        pBuffer->pBuffer[ 0 ] = packetType;
        pBuffer->pBuffer[ 1 ] = 3;
        pBuffer->pBuffer[ 2 ] = ( uint8_t ) ( packetId >> 8 );
        pBuffer->pBuffer[ 3 ] = ( uint8_t ) ( packetId & 0x00ffU );
        pBuffer->pBuffer[ 4 ] = 1;
    }

    return status;
}

/*-----------------------------------------------------------*/

static MQTTStatus_t handleOutgoingConnect( const void * buf,
                                           MQTTFixedBuffer_t * bufToSend,
                                           size_t * pSize )
{
    MQTTStatus_t status = MQTTSuccess;
    MQTTConnectInfo_t connectConfig = { 0 };

    LogDebug( ( "Processing outgoing CONNECT." ) );

    status = parseConnect( &connectConfig, buf );

    if( status == MQTTSuccess )
    {
        status = IotBleMqtt_SerializeConnect( &connectConfig, &bufToSend->pBuffer, pSize );
    }

    return status;
}

static MQTTStatus_t handleOutgoingPublish( MQTTBLEPublishInfo_t *pPublishInfo,
                                           const void * buf,
                                           MQTTFixedBuffer_t * bufToSend,
                                           size_t * pSize,
                                           size_t bytesToSend )
{
    MQTTStatus_t status = MQTTSuccess;

    LogDebug( ( "Processing outgoing PUBLISH." ) );

    if( pPublishInfo->pending == true )
    {
        configASSERT(( pPublishInfo->info.payloadLength == bytesToSend ));
        pPublishInfo->info.pPayload = buf;
        pPublishInfo->pending = false;
        
    }
    else
    {
        pPublishInfo->pending = parsePublish( buf, bytesToSend, pPublishInfo );
    }

    if( !pPublishInfo->pending )
    {
        status = IotBleMqtt_SerializePublish( &pPublishInfo->info, &bufToSend->pBuffer, pSize, pPublishInfo->packetIdentifier );
        if( pPublishInfo->info.pTopicName != NULL )
        {
            IotMqtt_FreeMessage( ( void * ) pPublishInfo->info.pTopicName );
        }
        memset( pPublishInfo, 0x00, sizeof( MQTTBLEPublishInfo_t ) );
    }
    else
    {
        bufToSend->pBuffer = NULL;
        *pSize = 0;
    }

    return status;
}


static MQTTStatus_t handleOutgoingPuback( const void * buf,
                                          MQTTFixedBuffer_t * bufToSend,
                                          size_t * pSize )
{
    MQTTStatus_t status = MQTTSuccess;
    uint16_t packetIdentifier = 0;
    MQTTPacketInfo_t pubackPacket;

    LogDebug( ( "Processing outgoing PUBACK." ) );

    pubackPacket.pRemainingData = ( uint8_t * ) buf;
    pubackPacket.type = MQTT_PACKET_TYPE_PUBACK;
    pubackPacket.remainingLength = 2U;

    status = MQTT_DeserializeAck( &pubackPacket, &packetIdentifier, NULL );

    if( status == MQTTSuccess )
    {
        status = IotBleMqtt_SerializePuback( packetIdentifier, &bufToSend->pBuffer, pSize );
    }

    return status;
}


static MQTTStatus_t handleOutgoingSubscribe( const void * buf,
                                             MQTTFixedBuffer_t * bufToSend,
                                             size_t * pSize )
{
    MQTTStatus_t status = MQTTSuccess;
    uint16_t packetIdentifier = 0;
    size_t subscriptionCount = 0;

    LogDebug( ( "Processing outgoing SUBSCRIBE." ) );

    status = parseSubscribe( &subscriptionCount, buf, &packetIdentifier, true );

    if( status == MQTTSuccess )
    {
        status = IotBleMqtt_SerializeSubscribe( _subscriptions, subscriptionCount, &bufToSend->pBuffer, pSize, &packetIdentifier );
    }

    return status;
}


static MQTTStatus_t handleOutgoingUnsubscribe( const void * buf,
                                               MQTTFixedBuffer_t * bufToSend,
                                               size_t * pSize )
{
    MQTTStatus_t status = MQTTSuccess;
    uint16_t packetIdentifier = 0;
    size_t subscriptionCount = 0;

    LogDebug( ( "Processing outgoing UNSUBSCRIBE." ) );

    status = parseSubscribe( &subscriptionCount, buf, &packetIdentifier, false );

    if( status == MQTTSuccess )
    {
        status = IotBleMqtt_SerializeUnsubscribe( _subscriptions, subscriptionCount, &bufToSend->pBuffer, pSize, &packetIdentifier );
    }

    return status;
}


static MQTTStatus_t handleOutgoingPingReq( MQTTFixedBuffer_t * bufToSend,
                                           size_t * pSize )
{
    MQTTStatus_t status = MQTTSuccess;

    LogDebug( ( "Processing outgoing PINGREQ." ) );

    *pSize = SIZE_OF_PING;
    status = IotBleMqtt_SerializePingreq( &bufToSend->pBuffer, pSize );
    return status;
}

static MQTTStatus_t handleOutgoingDisconnect( MQTTFixedBuffer_t * bufToSend,
                                              size_t * pSize )
{
    MQTTStatus_t status = MQTTSuccess;

    LogDebug( ( "Processing outgoing DISCONNECT." ) );

    *pSize = SIZE_OF_PING;
    /* Disconnect packets are always 2 bytes */
    status = IotBleMqtt_SerializeDisconnect( &bufToSend->pBuffer, pSize );
    return status;
}

/*-----------------------------------------------------------*/

static MQTTStatus_t handleIncomingConnack( StreamBufferHandle_t streamBuffer,
                                           MQTTPacketInfo_t * packet,
                                           MQTTFixedBuffer_t * pBuffer )
{
    MQTTStatus_t status = MQTTSuccess;

    LogDebug( ( "Processing incoming CONNACK from channel." ) );

    status = IotBleMqtt_DeserializeConnack( packet );

    if( status == MQTTSuccess )
    {
        /* Packet ID is not used in connack. */
        status = transportSerializeAck( pBuffer, packet->type, 0 );
    }

    if( status == MQTTSuccess )
    {
        ( void ) xStreamBufferSend( streamBuffer, pBuffer->pBuffer, SIZE_OF_SIMPLE_ACK, pdMS_TO_TICKS( RECV_TIMEOUT_MS ) );
    }

    return status;
}

static MQTTStatus_t handleIncomingPuback( StreamBufferHandle_t streamBuffer,
                                          MQTTPacketInfo_t * packet,
                                          MQTTFixedBuffer_t * pBuffer )
{
    MQTTStatus_t status = MQTTSuccess;
    uint16_t packetIdentifier = 0;

    LogDebug( ( "Processing incoming PUBACK from channel." ) );
    status = IotBleMqtt_DeserializePuback( packet, &packetIdentifier );

    if( status == MQTTSuccess )
    {
        status = transportSerializeAck( pBuffer, packet->type, packetIdentifier );
    }

    if( status == MQTTSuccess )
    {
        ( void ) xStreamBufferSend( streamBuffer, pBuffer->pBuffer, SIZE_OF_SIMPLE_ACK, pdMS_TO_TICKS( RECV_TIMEOUT_MS ) );
    }

    return status;
}

static MQTTStatus_t handleIncomingPublish( StreamBufferHandle_t streamBuffer,
                                           MQTTPacketInfo_t * packet,
                                           MQTTFixedBuffer_t * pBuffer )
{
    MQTTStatus_t status = MQTTSuccess;
    MQTTPublishInfo_t publishInfo;
    uint16_t packetIdentifier = 0;

    LogDebug( ( "Processing incoming PUBLISH from channel." ) );

    status = IotBleMqtt_DeserializePublish( packet, &publishInfo, &packetIdentifier );

    if( status == MQTTSuccess )
    {
        status = transportSerializePublish( streamBuffer, &publishInfo, packetIdentifier );
    }

    return status;
}


static MQTTStatus_t handleIncomingSuback( StreamBufferHandle_t streamBuffer,
                                          MQTTPacketInfo_t * packet,
                                          MQTTFixedBuffer_t * pBuffer )
{
    MQTTStatus_t status = MQTTSuccess;
    uint16_t packetIdentifier = 0;

    LogDebug( ( "Processing incoming SUBACK from channel." ) );
    status = IotBleMqtt_DeserializeSuback( packet, &packetIdentifier );

    if( status == MQTTSuccess )
    {
        status = transportSerializeSuback( pBuffer, packet->type, packetIdentifier );
    }

    if( status == MQTTSuccess )
    {
        ( void ) xStreamBufferSend( streamBuffer, pBuffer->pBuffer, SIZE_OF_SUB_ACK, pdMS_TO_TICKS( RECV_TIMEOUT_MS ) );
    }

    return status;
}

static MQTTStatus_t handleIncomingUnsuback( StreamBufferHandle_t streamBuffer,
                                            MQTTPacketInfo_t * packet,
                                            MQTTFixedBuffer_t * pBuffer )
{
    MQTTStatus_t status = MQTTSuccess;
    uint16_t packetIdentifier = 0;

    LogDebug( ( "Processing incoming UNSUBACK from channel." ) );
    status = IotBleMqtt_DeserializeUnsuback( packet, &packetIdentifier );

    if( status == MQTTSuccess )
    {
        status = transportSerializeAck( pBuffer, packet->type, packetIdentifier );
    }

    if( status == MQTTSuccess )
    {
        ( void ) xStreamBufferSend( streamBuffer, pBuffer->pBuffer, SIZE_OF_SIMPLE_ACK, pdMS_TO_TICKS( RECV_TIMEOUT_MS ) );
    }

    return status;
}


static MQTTStatus_t handleIncomingPingresp( StreamBufferHandle_t streamBuffer, 
                                            MQTTPacketInfo_t * packet,
                                            MQTTFixedBuffer_t * pBuffer )
{
    MQTTStatus_t status = MQTTSuccess;

    LogDebug( ( "Processing incoming PINGRESP from channel." ) );
    status = IotBleMqtt_DeserializePingresp( packet );

    if( status == MQTTSuccess )
    {
        status = transportSerializePingresp( pBuffer );
    }

    if( status == MQTTSuccess )
    {
        ( void ) xStreamBufferSend( streamBuffer, pBuffer->pBuffer, SIZE_OF_PING, pdMS_TO_TICKS( RECV_TIMEOUT_MS ) );
    }

    return status;
}

/*-----------------------------------------------------------*/


/**
 * @brief Transport interface write prototype.
 *
 * @param[in] context An opaque used by transport interface.
 * @param[in] buf A pointer to a buffer containing data to be sent out.
 * @param[in] bytesToWrite number of bytes to write from the buffer.
 */
int32_t IotBleMqttTransportSend( NetworkContext_t * pContext,
                                 const void * buf,
                                 size_t bytesToWrite )
{
    size_t bytesSend = 0;
    uint8_t packetType = ( *( ( uint8_t * ) buf ) & 0xF0 );
    MQTTStatus_t status = MQTTSuccess;
    MQTTFixedBuffer_t serializedBuf = { 0 };
    size_t packetSize = 0;

    /* The send function returns the CBOR bytes written, so need to return 0 or full amount of bytes sent. */
    int32_t MQTTBytesWritten = ( int32_t ) bytesToWrite;

    if( pContext->publishInfo.pending == true )
    {
        status = handleOutgoingPublish( &pContext->publishInfo, buf, &serializedBuf, &packetSize, bytesToWrite );
    }
    else
    {
        switch( packetType )
        {
            case CLIENT_PACKET_TYPE_CONNECT:
                status = handleOutgoingConnect( buf, &serializedBuf, &packetSize );
                break;

            case CLIENT_PACKET_TYPE_PUBLISH:
                status = handleOutgoingPublish( &pContext->publishInfo, buf, &serializedBuf, &packetSize, bytesToWrite );
                break;

            case CLIENT_PACKET_TYPE_PUBACK:
                status = handleOutgoingPuback( buf, &serializedBuf, &packetSize );
                break;

            case CLIENT_PACKET_TYPE_SUBSCRIBE:
                status = handleOutgoingSubscribe( buf, &serializedBuf, &packetSize );
                break;

            case CLIENT_PACKET_TYPE_UNSUBSCRIBE:
                status = handleOutgoingUnsubscribe( buf, &serializedBuf, &packetSize );
                break;

            case CLIENT_PACKET_TYPE_PINGREQ:
                status = handleOutgoingPingReq( &serializedBuf, &packetSize );
                break;

            case CLIENT_PACKET_TYPE_DISCONNECT:
                status = handleOutgoingDisconnect( &serializedBuf, &packetSize );
                break;

            /* QoS 2 cases, currently not supported by BLE */
            case CLIENT_PACKET_TYPE_PUBREC:
            case CLIENT_PACKET_TYPE_PUBREL:
            case CLIENT_PACKET_TYPE_PUBCOMP:
                status = MQTTSendFailed;
                LogError( ( "Only Qos 0 and 1 are supported over BLE." ) );
                break;

            /* Client tries to send a server to client only packet */
            default:
                status = MQTTBadParameter;
                LogError( ( "A server to client only packet was sent. Check packet type or ensure Qos < 2." ) );
                LogError( ( "Your packet type was %i", packetType ) );
                break;
        }
    }

    if( status == MQTTSuccess )
    {
        if( packetSize > 0 )
        {
            bytesSend = IotBleDataTransfer_Send( pContext->pChannel, serializedBuf.pBuffer, packetSize );
            if( bytesSend != packetSize )
            {
                LogError( ( "Cannot sent %d bytes through the BLE channel, sent %d bytes.\r\n", packetSize, bytesSend ) );
                MQTTBytesWritten = 0;
            }
            IotMqtt_FreeMessage( serializedBuf.pBuffer );
        }
    }
    else
    {
        /* The user would have already seen a log for the previous error */
        MQTTBytesWritten = 0;
    }

    return MQTTBytesWritten;
}

MQTTStatus_t IotBleMqttTransportAcceptData( const NetworkContext_t * pContext )
{
    MQTTStatus_t status = MQTTSuccess;
    MQTTPacketInfo_t packet;
    MQTTFixedBuffer_t buffer;

    /* Sub ack is the largest packet size expected except for publish, which has its own buffer allocation */
    uint8_t sharedBuffer[ SIZE_OF_SUB_ACK ];

    buffer.pBuffer = sharedBuffer;
    buffer.size = SIZE_OF_SUB_ACK;

    packet.type = IotBleMqtt_GetPacketType( pContext->pChannel );
    IotBleDataTransfer_PeekReceiveBuffer( pContext->pChannel, ( const uint8_t ** ) &packet.pRemainingData, &packet.remainingLength );

    LogDebug( ( "Receiving a packet from the server." ) );

    switch( packet.type )
    {
        case MQTT_PACKET_TYPE_CONNACK:
            status = handleIncomingConnack( pContext->xStreamBuffer, &packet, &buffer );
            break;

        case MQTT_PACKET_TYPE_PUBLISH:
            status = handleIncomingPublish( pContext->xStreamBuffer, &packet, &buffer );
            break;

        case MQTT_PACKET_TYPE_PUBACK:
            status = handleIncomingPuback( pContext->xStreamBuffer, &packet, &buffer );
            break;

        case MQTT_PACKET_TYPE_SUBACK:
            status = handleIncomingSuback( pContext->xStreamBuffer, &packet, &buffer );
            break;

        case MQTT_PACKET_TYPE_UNSUBACK:
            status = handleIncomingUnsuback( pContext->xStreamBuffer, &packet, &buffer );
            break;

        case MQTT_PACKET_TYPE_PINGRESP:
            status = handleIncomingPingresp( pContext->xStreamBuffer, &packet, &buffer );
            break;

        /* QoS 2 cases, currently not supported by BLE */
        case MQTT_PACKET_TYPE_PUBREC:
        case MQTT_PACKET_TYPE_PUBREL:
        case MQTT_PACKET_TYPE_PUBCOMP:
            status = MQTTRecvFailed;

            LogError( ( "Only Qos 0 and 1 are supported over BLE." ) );
            break;

        /* Server tries to send a client to server only packet */
        default:
            LogError( ( "Client received a client to server only packet." ) );
            status = MQTTBadParameter;
            break;
    }

    if( status != MQTTSuccess )
    {
        LogError( ( "An error occured when receiving data from the channel. No data was recorded." ) );
    }
    else
    {
        /* Flush the data from the channel */
        ( void ) IotBleDataTransfer_Receive( pContext->pChannel, NULL, packet.remainingLength );
    }
    return status;
}


/**
 * @brief Transport interface read prototype.
 *
 * @param[in] context An opaque used by transport interface.
 * @param[out] buf A pointer to a buffer where incoming data will be stored.
 * @param[in] bytesToRead number of bytes to read from the transport layer.
 */
int32_t IotBleMqttTransportReceive( NetworkContext_t * pContext,
                                    void * buf,
                                    size_t bytesToRead )
{
    return ( int32_t ) xStreamBufferReceive( pContext->xStreamBuffer, buf, bytesToRead, pdMS_TO_TICKS( RECV_TIMEOUT_MS ) );
}
