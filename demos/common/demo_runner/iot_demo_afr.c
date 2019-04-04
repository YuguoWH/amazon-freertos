/*
 * Copyright (C) 2018 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
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

/**
 * @file iot_demo_afr.c
 * @brief Generic demo runner for C SDK libraries on Amazon FreeRTOS.
 */

/* Build using a config header, if provided. */
#ifdef IOT_CONFIG_FILE
    #include IOT_CONFIG_FILE
#endif

/* FreeRTOS demo configuration. */
#include "aws_demo_config.h"

/* Amazon FreeRTOS credentials include. */
#include "aws_clientcredential.h"

/* Demo logging include. */
#include "iot_demo_logging.h"

/* Amazon FreeRTOS network include. */
#include "platform/iot_network_afr.h"

/* Network manager include */
#include "iot_network_manager_private.h"

/* Platform includes */
#include "platform/iot_threads.h"

/*-----------------------------------------------------------*/

/**
 * @brief All C SDK demo functions have this signature.
 */
typedef int (* _demoFunction_t)( bool awsIotMqttMode,
                                 const char * pIdentifier,
                                 void * pNetworkServerInfo,
                                 void * pNetworkCredentialInfo,
                                 const IotNetworkInterface_t * pNetworkInterface );

typedef void (*_onNetworkConnected_t )( bool awsIotMqttMode,
                                       const char *pIdentifier,
                                       void *pNetworkServerInfo,
                                       void *pNetworkCredentialInfo,
                                       const IotNetworkInterface_t *pNetworkInterface );

typedef void ( *_onNetworkDisconnected_t )( const IotNetworkInterface_t * pNetworkInteface );


typedef struct _demoContext
{

    /* Network types for the demo */
    uint32_t networkTypes;
    uint32_t connectedNetwork;

    /* Function pointers for the demo */
    _demoFunction_t demoFn;
    _onNetworkConnected_t onNetworkConnectedFn;
    _onNetworkDisconnected_t onNetworkDisconnectedFn;

    /* Semaphore used to synchronize internally */
    IotSemaphore_t networkSemaphore;

} _demoContext_t;

/*-----------------------------------------------------------*/

/* C SDK demo functions. */
extern int RunMqttDemo( bool awsIotMqttMode,
                        const char * pIdentifier,
                        void * pNetworkServerInfo,
                        void * pNetworkCredentialInfo,
                        const IotNetworkInterface_t * pNetworkInterface );
extern int RunShadowDemo( bool awsIotMqttMode,
                          const char * pIdentifier,
                          void * pNetworkServerInfo,
                          void * pNetworkCredentialInfo,
                          const IotNetworkInterface_t * pNetworkInterface );

extern int RunBleMqttEchoDemo( bool awsIotMqttMode,
                               const char *pIdentifier,
                               void *pNetworkServerInfo,
                               void *pNetworkCredentialInfo,
                               const IotNetworkInterface_t *pNetworkInterface );

extern void BleMqttEchoDemoOnNetworkConnected( bool awsIotMqttMode,
                                       const char *pIdentifier,
                                       void *pNetworkServerInfo,
                                       void *pNetworkCredentialInfo,
                                       const IotNetworkInterface_t *pNetworkInterface );

extern void BleMqttEchoDemoOnNetworkDisconnected( const IotNetworkInterface_t * pNetworkInteface );

/*-----------------------------------------------------------*/


static void _onNetworkStateChangeCallback( uint32_t networkType, AwsIotNetworkState_t state,  void *pContext )
{
    IotNetworkServerInfoAfr_t serverInfo = AWS_IOT_NETWORK_SERVER_INFO_AFR_INITIALIZER;
    IotNetworkCredentialsAfr_t credentials = AWS_IOT_NETWORK_CREDENTIALS_AFR_INITIALIZER;
    const IotNetworkInterface_t *pNetworkInterface;
    bool awsIotMqttMode = false;

    _demoContext_t* pDemoContext = ( _demoContext_t* ) pContext;
    if( state == eNetworkStateEnabled )
    {
        if( pDemoContext->connectedNetwork == AWSIOT_NETWORK_TYPE_NONE )
        {

            IotSemaphore_Post( &pDemoContext->networkSemaphore );
            
            if( pDemoContext->onNetworkConnectedFn != NULL )
            {
                pNetworkInterface = AwsIotNetworkManager_GetNetworkInterface( networkType );

                awsIotMqttMode = ( networkType != AWSIOT_NETWORK_TYPE_BLE );
                /* ALPN only works over port 443. Disable it otherwise. */
                if (serverInfo.port != 443)
                {
                    credentials.pAlpnProtos = NULL;
                }

                pDemoContext->onNetworkConnectedFn(awsIotMqttMode,
                                                   clientcredentialIOT_THING_NAME,
                                                   &serverInfo,
                                                   &credentials,
                                                   pNetworkInterface);
            }
        }
    }
    else if( state == eNetworkStateDisabled )
    {
        if( pDemoContext->connectedNetwork == networkType )
        {
            pDemoContext->connectedNetwork = AWSIOT_NETWORK_TYPE_NONE;
            if (pDemoContext->onNetworkDisconnectedFn != NULL)
            {
                pNetworkInterface = AwsIotNetworkManager_GetNetworkInterface(networkType);
                pDemoContext->onNetworkDisconnectedFn(pNetworkInterface);
            }
        }
    }    
}

static uint32_t _blockForAvailableNetwork( _demoContext_t *pContext )
{
    IotSemaphore_Wait( &pContext->networkSemaphore );
    
    return (AwsIotNetworkManager_GetConnectedNetworks() & pContext->networkTypes);
}

static void _runDemoTask( void *pArgument )
{
    /* On Amazon FreeRTOS, credentials and server info are defined in a header
     * and set by the initializers. */
    IotNetworkServerInfoAfr_t serverInfo = AWS_IOT_NETWORK_SERVER_INFO_AFR_INITIALIZER;
    IotNetworkCredentialsAfr_t credentials = AWS_IOT_NETWORK_CREDENTIALS_AFR_INITIALIZER;
    const IotNetworkInterface_t *pNetworkInterface = NULL;
    uint32_t availableNetworkTypes = AWSIOT_NETWORK_TYPE_NONE;
    _demoContext_t* pDemoContext = ( _demoContext_t* ) pArgument;
    bool ret = true;
    bool awsIotMqttMode = false;
    IotNetworkManagerSubscription_t subscription = IOT_NETWORK_MANAGER_SUBSCRIPTION_INITIALIZER;
    

    /* Create semaphore to signal network is available */
    ret = IotSemaphore_Create( &pDemoContext->networkSemaphore, 0, 1 );

    if( ret == true )
    {
        /* Subscribe for network state changes from Network Manager */
        if ( AwsIotNetworkManager_SubscribeForStateChange(
                pDemoContext->networkTypes,
                _onNetworkStateChangeCallback,
                pDemoContext,
                &subscription ) != pdTRUE)
        {
            IotLogError("Failed to subscribe with network manager for network state change.");
            ret = false;
        }
    }

    if( ret == true )
    {
        /* Check for available networks, if none available block untill a network is available. */

        availableNetworkTypes = ( AwsIotNetworkManager_GetConnectedNetworks() & pDemoContext->networkTypes );
        
        if (availableNetworkTypes == AWSIOT_NETWORK_TYPE_NONE)
        {
            /* No available Networks. Block for a network to be available. */
            availableNetworkTypes = _blockForAvailableNetwork(pDemoContext);    
        }    
    }

    if( ret == true )
    {
        /* There are available networks at this point. Follow the preference order Wi-Fi, BLE, Ethernet etc.. */

        if( ( availableNetworkTypes & AWSIOT_NETWORK_TYPE_WIFI ) == AWSIOT_NETWORK_TYPE_WIFI )
        {
            pDemoContext->connectedNetwork = AWSIOT_NETWORK_TYPE_WIFI;
            pNetworkInterface = AwsIotNetworkManager_GetNetworkInterface( AWSIOT_NETWORK_TYPE_WIFI );
        
            /* ALPN only works over port 443. Disable it otherwise. */
            if (serverInfo.port != 443)
            {
                credentials.pAlpnProtos = NULL;
            }
            
            /* Set IOT Mqtt Mode to true to not disable keep alive for a TCP/IP network */
            awsIotMqttMode = true;
        }
        else if( ( availableNetworkTypes & AWSIOT_NETWORK_TYPE_BLE ) == AWSIOT_NETWORK_TYPE_BLE )
        {
            pDemoContext->connectedNetwork = AWSIOT_NETWORK_TYPE_BLE;
            pNetworkInterface = AwsIotNetworkManager_GetNetworkInterface( AWSIOT_NETWORK_TYPE_BLE );

            /* Set AWS Iot Mqtt mode to false to disable keep alive for Non TCP network like Bluetooth */
            awsIotMqttMode = false;
        }
        else
        {
            /* Other network types are not supported */
            ret = false;
        }
    }
    
    if( ret == true )
    {
        /* Run the demo. */
        pDemoContext->demoFn(awsIotMqttMode,
                             clientcredentialIOT_THING_NAME,
                             &serverInfo,
                             &credentials,
                             pNetworkInterface);

        /* Report heap usage. */
        IotLogInfo("Demo minimum ever free heap: %lu bytes.",
                   (unsigned long)xPortGetMinimumEverFreeHeapSize());

    }

    /* Remove network manager subscription */
    if( subscription != IOT_NETWORK_MANAGER_SUBSCRIPTION_INITIALIZER )
    {
        AwsIotNetworkManager_RemoveSubscription( subscription );
    }

    /* Delete Network available semaphore */
    if( ret == true )
    {
        IotSemaphore_Destroy( &pDemoContext->networkSemaphore );
    }
    
     vTaskDelete(NULL);
}

/*-----------------------------------------------------------*/

void vStartMQTTDemo( void )
{
    static _demoContext_t mqttDemoContext =
    {
        .networkTypes                 = AWSIOT_NETWORK_TYPE_WIFI,
        .connectedNetwork             = AWSIOT_NETWORK_TYPE_NONE,
        .demoFn                       = RunMqttDemo,
        .onNetworkConnectedFn         = NULL,
        .onNetworkDisconnectedFn      = NULL
    };

    ( void ) xTaskCreate( _runDemoTask,
                          "MQTTDemo",
                          democonfigMQTT_SUB_PUB_TASK_STACK_SIZE,
                          &mqttDemoContext,
                          democonfigMQTT_SUB_PUB_TASK_PRIORITY,
                          NULL );
}



void vStartMQTTBLEEchoDemo( void )
{
    static _demoContext_t mqttBleEchoDemoContext =
    {
        .networkTypes                  = AWSIOT_NETWORK_TYPE_BLE | AWSIOT_NETWORK_TYPE_WIFI,
        .connectedNetwork              = AWSIOT_NETWORK_TYPE_NONE,
        .demoFn                        = RunBleMqttEchoDemo,
        .onNetworkConnectedFn          = BleMqttEchoDemoOnNetworkConnected,
        .onNetworkDisconnectedFn       = BleMqttEchoDemoOnNetworkDisconnected,
    };

    ( void ) xTaskCreate( _runDemoTask,
                          "BLEMQTTEchoDemo",
                          democonfigMQTT_BLE_ECHO_TASK_STACK_SIZE,
                          &mqttBleEchoDemoContext,
                          democonfigMQTT_BLE_ECHO_TASK_PRIORITY,
                          NULL );
}

/*-----------------------------------------------------------*/

void vStartShadowDemo( void )
{
    static _demoContext_t shadowDemoContext =
    {
        .networkTypes                  = AWSIOT_NETWORK_TYPE_WIFI,
        .connectedNetwork              = AWSIOT_NETWORK_TYPE_NONE,
        .demoFn                        = RunShadowDemo,
        .onNetworkConnectedFn          = NULL,
        .onNetworkDisconnectedFn       = NULL,
    };

    ( void ) xTaskCreate( _runDemoTask,
                          "ShadowDemo",
                          democonfigSHADOW_DEMO_TASK_STACK_SIZE,
                          &shadowDemoContext,
                          democonfigSHADOW_DEMO_TASK_PRIORITY,
                          NULL );
}

/*-----------------------------------------------------------*/