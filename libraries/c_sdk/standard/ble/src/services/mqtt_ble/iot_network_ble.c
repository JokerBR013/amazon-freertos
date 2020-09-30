/*
 * FreeRTOS BLE V2.1.0
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
 *
 * http://aws.amazon.com/freertos
 * http://www.FreeRTOS.org
 */

/**
 * @file iot_network_ble.c
 * @brief Implementation of network interface for BLE. This implementation is provided as a SHIM
 * layer of backwards compatibility for the V4 MQTT library.
 */

#include  <stdbool.h>

/* Config files needs to be defined first. */
#include "iot_config.h"
#include "iot_ble_config.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "stream_buffer.h"

#include "platform/iot_network_ble.h"
#include "iot_ble_data_transfer.h"
#include "iot_ble_mqtt_transport.h"

/* Configure logs for the functions in this file. */
#ifdef IOT_LOG_LEVEL_GLOBAL
    #define LIBRARY_LOG_LEVEL    IOT_LOG_LEVEL_GLOBAL
#else
    #define LIBRARY_LOG_LEVEL    IOT_LOG_NONE
#endif

#define LIBRARY_LOG_NAME         ( "NET_BLE" )
#include "iot_logging_setup.h"

/**
 * @brief Structure holds the context associated with a ble connection.
 */
typedef struct IotBleNetworkConnection
{
    NetworkContext_t xContext;                  /* Network Context structure to hold the ble transport channel */
    IotNetworkReceiveCallback_t pCallback;     /* Callback registered by user to get notified of receipt of new data. */
    void * pUserContext;                       /* User context associated with the callback registered. */
    SemaporeHandle_t xChannelOpened;           /* Semaphore to wait on if the BLE transfer channel is opened. */
} IotBleNetworkConnection_t;

/**
 * @brief An implementation of #IotNetworkInterface_t::create using
 * transport interface for BLE.
 */
static IotNetworkError_t IotNetworkBle_Create( void * pConnectionInfo,
                                        void * pCredentialInfo,
                                        void ** pConnection );

/**
 * @brief An implementation of #IotNetworkInterface_t::setReceiveCallback using
 * transport interface for BLE.
 */
static IotNetworkError_t IotNetworkBle_SetReceiveCallback( void * pConnection,
                                                    IotNetworkReceiveCallback_t receiveCallback,
                                                    void * pContext );

/**
 * @brief An implementation of #IotNetworkInterface_t::send using
 * transport interface for BLE.
 */
static size_t IotNetworkBle_Send( void * pConnection,
                           const uint8_t * pMessage,
                           size_t messageLength );

/**
 * @brief An implementation of #IotNetworkInterface_t::receive using
 * transport interface for BLE.
 */
static size_t IotNetworkBle_Receive( void * pConnection,
                              uint8_t * pBuffer,
                              size_t bytesRequested );

/**
 * @brief An implementation of #IotNetworkInterface_t::close fusing
 * transport interface for BLE.
 */
static IotNetworkError_t IotNetworkBle_Close( void * pConnection );

/**
 * @brief An implementation of #IotNetworkInterface_t::destroy using
 * transport interface for BLE.
 */
static IotNetworkError_t IotNetworkBle_Destroy( void * pConnection );



static void _callback( IotBleDataTransferChannelEvent_t event,
                       IotBleDataTransferChannel_t * pChannel,
                       void * pContext )
{
    IotBleNetworkConnection_t * pBleConnection = ( IotBleNetworkConnection_t * ) pContext;
    MQTTStatus_t xStatus;

    switch( event )
    {
        case IOT_BLE_DATA_TRANSFER_CHANNEL_OPENED:
            xSemaphoreGive( pBLEConnection->xChannelOpened );
            break;

        case IOT_BLE_DATA_TRANSFER_CHANNEL_DATA_RECEIVED:
            xStatus = IotBleMqttTransportAcceptData( &pBleConnection->xContext );
            configASSERT( xStatus == MQTTSuccess );
            if( pBleConnection->pCallback != NULL )
            {
                pBleConnection->pCallback( pBleConnection, pBleConnection->pUserContext );
            }
            break;

        default:
            break;
    }
}

/**
 * @brief An #IotNetworkInterface_t that uses the functions in this file.
 */
const IotNetworkInterface_t IotNetworkBle =
{
    .create             = IotNetworkBle_Create,
    .setReceiveCallback = IotNetworkBle_SetReceiveCallback,
    .send               = IotNetworkBle_Send,
    .receive            = IotNetworkBle_Receive,
    .close              = IotNetworkBle_Close,
    .destroy            = IotNetworkBle_Destroy
};

IotNetworkError_t IotNetworkBle_Create( void * pConnectionInfo,
                                        void * pCredentialInfo,
                                        void ** pConnection )
{
    IotNetworkError_t status = IOT_NETWORK_FAILURE;
    IotBleNetworkConnection_t * pBleConnection = IotNetwork_Malloc( sizeof( IotBleNetworkConnection_t ) );

    /* Unused parameters */
    ( void ) pConnectionInfo;
    ( void ) pCredentialInfo;

    if( pBleConnection != NULL )
    {
        pBleConnection->pChannel = IotBleDataTransfer_Open( IOT_BLE_DATA_TRANSFER_SERVICE_TYPE_MQTT );

        if( pChannel != NULL )
        {
            pBleConnection->pChannel = pChannel;

            if( IotSemaphore_Create( &pBleConnection->ready, 0, 1 ) == true )
            {
                IotBleDataTransfer_SetCallback( pChannel, _callback, pBleConnection );

                if( IotSemaphore_TimedWait( &pBleConnection->ready, IOT_BLE_MQTT_CREATE_CONNECTION_WAIT_MS ) == true )
                {
                    ( *pConnection ) = &pBleConnection->pChannel;
                    status = IOT_NETWORK_SUCCESS;
                }
                else
                {
                    IotLogError( "Failed to create BLE network connection after %d milliseconds.", IOT_BLE_MQTT_CREATE_CONNECTION_WAIT_MS );
                    IotBleDataTransfer_Reset( pChannel );
                    IotSemaphore_Destroy( &pBleConnection->ready );
                }
            }
            else
            {
                IotLogError( "Failed to create BLE network connection, cannot create network connection semaphore." );
            }
        }

        if( status != IOT_NETWORK_SUCCESS )
        {
            IotNetwork_Free( pBleConnection );
        }
    }

    return status;
}

IotNetworkError_t IotNetworkBle_SetReceiveCallback( void * pConnection,
                                                    IotNetworkReceiveCallback_t receiveCallback,
                                                    void * pContext )
{
    _bleNetworkConnection_t * pBleConnection = _CONTAINER( _bleNetworkConnection_t, pConnection, pChannel );
    IotNetworkError_t status = IOT_NETWORK_FAILURE;

    if( pBleConnection != NULL )
    {
        pBleConnection->pCallback = receiveCallback;
        pBleConnection->pContext = pContext;
        status = IOT_NETWORK_SUCCESS;
    }

    return status;
}

size_t IotNetworkBle_Send( void * pConnection,
                           const uint8_t * pMessage,
                           size_t messageLength )
{
    _bleNetworkConnection_t * pBleConnection = _CONTAINER( _bleNetworkConnection_t, pConnection, pChannel );

    return IotBleDataTransfer_Send( pBleConnection->pChannel, pMessage, messageLength );
}

size_t IotNetworkBle_Receive( void * pConnection,
                              uint8_t * pBuffer,
                              size_t bytesRequested )
{
    _bleNetworkConnection_t * pBleConnection = _CONTAINER( _bleNetworkConnection_t, pConnection, pChannel );

    return IotBleDataTransfer_Receive( pBleConnection->pChannel, pBuffer, bytesRequested );
}

IotNetworkError_t IotNetworkBle_Close( void * pConnection )
{
    _bleNetworkConnection_t * pBleConnection = _CONTAINER( _bleNetworkConnection_t, pConnection, pChannel );

    IotBleDataTransfer_Close( pBleConnection->pChannel );
    return IOT_NETWORK_SUCCESS;
}


IotNetworkError_t IotNetworkBle_Destroy( void * pConnection )
{
    _bleNetworkConnection_t * pBleConnection = _CONTAINER( _bleNetworkConnection_t, pConnection, pChannel );

    IotBleDataTransfer_Reset( pBleConnection->pChannel );
    IotSemaphore_Destroy( &pBleConnection->ready );
    IotNetwork_Free( pBleConnection );

    return IOT_NETWORK_SUCCESS;
}
