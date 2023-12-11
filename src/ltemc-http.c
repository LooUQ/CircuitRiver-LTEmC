/** ***************************************************************************
  @file 
  @brief Modem HTTP(S) communication features/services.

  @author Greg Terrell, LooUQ Incorporated

  \loouq
-------------------------------------------------------------------------------

LooUQ-LTEmC // Software driver for the LooUQ LTEm series cellular modems.
Copyright (C) 2017-2023 LooUQ Incorporated

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
Also add information on how to contact you by electronic and paper mail.

**************************************************************************** */


#include <lq-embed.h>
#define LOG_LEVEL LOGLEVEL_DBG
//#define DISABLE_ASSERTS                   // ASSERT/ASSERT_W enabled by default, can be disabled 
#define SRCFILE "HTT"                       // create SRCFILE (3 char) MACRO for lq-diagnostics ASSERT

#define ENABLE_DIAGPRINT                    // expand DPRINT into debug output
//#define ENABLE_DIAGPRINT_VERBOSE            // expand DPRINT and DPRINT_V into debug output
//#define ENABLE_ASSERT
// #include <lqdiag.h>

#include "ltemc-internal.h"
#include "ltemc-http.h"

extern ltemDevice_t g_lqLTEM;

#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#define MAX(X, Y) (((X) > (Y)) ? (X) : (Y))


/* Local Static Functions
------------------------------------------------------------------------------------------------------------------------- */
static resultCode_t S__httpGET(httpCtrl_t *httpCtrl, const char* relativeUrl, httpRequest_t* customRequest, bool returnResponseHdrs);
static resultCode_t S__httpPOST(httpCtrl_t *httpCtrl, const char *relativeUrl, httpRequest_t* request, const char *postData, uint16_t postDataSz, bool returnResponseHdrs);

static uint16_t S__setUrl(const char *host, const char *relative);
static cmdParseRslt_t S__httpGetStatusParser();
static cmdParseRslt_t S__httpPostStatusParser();
static cmdParseRslt_t S__httpReadFileStatusParser();
static cmdParseRslt_t S__httpPostFileStatusParser();
static uint16_t S__parseResponseForHttpStatus(httpCtrl_t *httpCtrl, const char *responseTail);
static resultCode_t S__httpRxHndlr();


/* Public Functions
------------------------------------------------------------------------------------------------------------------------- */

/**
 *	@brief Create a HTTP(s) control structure to manage web communications. 
 */
void http_initControl(httpCtrl_t *httpCtrl, dataCntxt_t dataCntxt, httpRecv_func recvCallback)
{
    ASSERT(httpCtrl != NULL && recvCallback != NULL);
    ASSERT(dataCntxt < dataCntxt__cnt);

    g_lqLTEM.streams[dataCntxt] = httpCtrl;

    memset(httpCtrl, 0, sizeof(httpCtrl_t));
    httpCtrl->dataCntxt = dataCntxt;
    httpCtrl->streamType = streamType_HTTP;
    httpCtrl->appRecvDataCB = (appRcvProto_func)recvCallback;
    httpCtrl->dataRxHndlr = S__httpRxHndlr;

    httpCtrl->requestState = httpState_idle;
    httpCtrl->httpStatus = resultCode__unknown;
    httpCtrl->pageCancellation = false;
    httpCtrl->useTls = false;
    httpCtrl->timeoutSec = http__defaultTimeoutBGxSec;
    httpCtrl->defaultBlockSz = bbffr_getCapacity(g_lqLTEM.iop->rxBffr) / 4;
    // httpCtrl->cstmHdrsBffr = NULL;
    // httpCtrl->cstmHdrsBffrSz = 0;
    httpCtrl->httpStatus = 0xFFFF;
}


/**
 *	@brief Set host connection characteristics. 
 */
void http_setConnection(httpCtrl_t *httpCtrl, const char *hostUrl, uint16_t hostPort)
{
    ASSERT(strncmp(hostUrl, "HTTP", 4) == 0 || strncmp(hostUrl, "http", 4) == 0);
    ASSERT(hostPort == 0 || hostPort >= 80);

    strncpy(httpCtrl->hostUrl, hostUrl, sizeof(httpCtrl->hostUrl));

    httpCtrl->useTls = ((httpCtrl->hostUrl)[4] == 'S' || (httpCtrl->hostUrl)[4] == 's');
    if (hostPort == 0)
    {
        httpCtrl->hostPort = httpCtrl->useTls ? 443 : 80;                  // if hostPort default, set from URL prefix
    }
}


/**
 * @brief Creates a base HTTP request that can be appended with custom headers.
 */
httpRequest_t http_createRequest(httpRequestType_t reqstType, const char* host, const char* relativeUrl, char* reqstBffr, uint16_t reqstBffrSz)
{
    ASSERT(strlen(host));
    ASSERT(strlen(relativeUrl));

    httpRequest_t httpRqst = { .requestBuffer = reqstBffr, .requestBuffersz = reqstBffrSz, .contentLen = 0, .headersLen = 0};
    memset(reqstBffr, 0, reqstBffrSz);

    if (COMPARES(memcmp(host, "http", 4)) || COMPARES(memcmp(host, "HTTP", 4)))         // allow for proto in host URL
    {
        host = (host, ':', strlen(host)) + 3;
    }

    if (reqstType == httpRequestType_GET)
        strcat(reqstBffr, "GET ");
    else if (reqstType == httpRequestType_POST)
        strcat(reqstBffr, "POST ");
    else
    {
        strcat(reqstBffr, "INVALID_TYPE");
        return httpRqst;
    }

    strcat(reqstBffr, relativeUrl);
    strcat(reqstBffr, " HTTP/1.1\r\nHost: ");
    strcat(reqstBffr, host);

    httpRqst.requestBuffer == reqstBffr;
    return httpRqst;
}


/**
 * @brief Adds common HTTP headers to a custom headers buffer.
 */
void http_addCommonHdrs(httpRequest_t* request, httpHeaderMap_t headerMap)
{
    ASSERT(headerMap > 0);
    ASSERT(request->contentLen == 0);                                                       // headers section still open to additions
    ASSERT(*(request->requestBuffer + strlen(request->requestBuffer) - 2) = '\r');          // existing request ends in \r\n
    ASSERT(strlen(request->requestBuffer) + http__commandHdrSz < request->requestBuffersz); // 102 all buffers below could fit

    if (headerMap & httpHeaderMap_accept > 0 || headerMap == httpHeaderMap_all)
    {
        strcat(request->requestBuffer, "Accept: */*\r\n");                                  // 13 = "Accept: */*\r\n" 
    }
    if (headerMap & httpHeaderMap_userAgent > 0 || headerMap == httpHeaderMap_all)
    {
        strcat(request->requestBuffer, "User-Agent: QUECTEL_MODULE\r\n");                   // 28 = "User-Agent: QUECTEL_BGx\r\n"
    }
    if (headerMap & httpHeaderMap_connection > 0 || headerMap == httpHeaderMap_all)
    {
        strcat(request->requestBuffer, "Connection: Keep-Alive\r\n");                       // 24 = "Connection: Keep-Alive\r\n"
    }
    if (headerMap & httpHeaderMap_contentType > 0 || headerMap == httpHeaderMap_all)
    {
        strcat(request->requestBuffer, "Content-Type: application/octet-stream\r\n");       // 40 = "Content-Type: application/octet-stream\r\n"
    }
}


/**
 * @brief Adds a basic authorization header to a headers buffer.
 */
void http_addBasicAuthHdr(httpRequest_t* request, const char *user, const char *pw)
{
    char toEncode[80];
    char b64str[120];

    ASSERT(user != NULL);
    ASSERT(pw != NULL);
    ASSERT(request->contentLen == 0);                                                           // headers section still open to additions
    ASSERT(*(request->requestBuffer + strlen(request->requestBuffer) - 2) = '\r');              // existing request ends in \r\n

    strcat(toEncode, user);
    strcat(toEncode, ":");
    strcat(toEncode, pw);
    binToB64(b64str, toEncode, strlen(toEncode));                                               // endcode credentials to Base64 string
    ASSERT(strlen(request->requestBuffer) + strlen(b64str) + 20 < request->requestBuffersz);    // "Authentication: " + "\r\n" = length 20

    strcat(request->requestBuffer, "Authentication: ");
    strcat(request->requestBuffer, b64str);
    strcat(request->requestBuffer, "\r\n");                                                     // new header ends in correct EOL
}


/**
 * @brief Helper to compose a generic header and add it to the headers collection being composed.
 */
void http_addHeader(httpRequest_t* request, const char *key, const char *val)
{
    ASSERT(key != NULL);
    ASSERT(val != NULL);
    ASSERT(request->contentLen == 0);                                                           // headers section still open to additions
    ASSERT(*(request->requestBuffer + strlen(request->requestBuffer) - 2) = '\r');              // existing request ends in \r\n

    uint8_t newHdrSz = strlen(key) + 2 + strlen(val) + 2;                                       // <key>: <val>\r\n
    ASSERT(strlen(request->requestBuffer) + newHdrSz < request->requestBuffersz);               // new header fits

    strcat(request->requestBuffer, key);
    strcat(request->requestBuffer, ": ");
    strcat(request->requestBuffer, val);
    strcat(request->requestBuffer, "\r\n");                                                    // new header ends in correct EOL
}


/**
 * @brief Helper to compose a generic header and add it to the headers collection being composed.
 */
void http_addPostData(httpRequest_t* request, const char *postData, uint16_t postDataSz)
{
    ASSERT(postData != NULL);
    ASSERT(*(request->requestBuffer + strlen(request->requestBuffer) - 2) = '\r');              // existing request ends in \r\n

    if (request->contentLen == 0)                                                              // finalize/close headers to additional changes
    {
        strcat(request->requestBuffer, "Content-Length:     0\r\n\r\n");
        request->headersLen = strlen(request->requestBuffer);
    
        ASSERT(request->headersLen + postDataSz < request->requestBuffersz);                    // request w/ headers + postDataSz fits
    }

    memcpy(request->requestBuffer + request->headersLen + request->contentLen, postData, postDataSz);
    request->contentLen += postDataSz;
}


/* ------------------------------------------------------------------------------------------------
 *  Request and Response Section 
 * --------------------------------------------------------------------------------------------- */

/**
 *	@brief Perform HTTP GET request. 
 *  -----------------------------------------------------------------------------------------------
 */
resultCode_t http_get(httpCtrl_t *httpCtrl, const char* relativeUrl, bool returnResponseHdrs)
{
    return S__httpGET(httpCtrl, relativeUrl, NULL, returnResponseHdrs);
}


/**
 *	@brief Performs a custom (headers) GET request.
 *  -----------------------------------------------------------------------------------------------
 */
resultCode_t http_getCustomRequest(httpCtrl_t *httpCtrl, const char* relativeUrl, httpRequest_t* customRequest, bool returnResponseHdrs)
{
    return S__httpGET(httpCtrl, relativeUrl, customRequest, returnResponseHdrs);
}


/**
 *	@brief Performs HTTP GET web request.
 *  -----------------------------------------------------------------------------------------------
 */
static resultCode_t S__httpGET(httpCtrl_t *httpCtrl, const char* relativeUrl, httpRequest_t* customRequest, bool returnResponseHdrs)
{
    httpCtrl->requestState = httpState_idle;
    httpCtrl->httpStatus = resultCode__unknown;
    strcpy(httpCtrl->requestType, "GET");
    resultCode_t rslt;

    if (ATCMD_awaitLock(httpCtrl->timeoutSec))
    {
        if (returnResponseHdrs)
        {
            atcmd_invokeReuseLock("AT+QHTTPCFG=\"responseheader\",%d",  (int)(httpCtrl->returnResponseHdrs));
            rslt = atcmd_awaitResultWithOptions(atcmd__defaultTimeout, NULL);
            if (rslt != resultCode__success)
            {
                atcmd_close();
                return rslt;
            }
        }

        if (httpCtrl->useTls)
        {
            // AT+QHTTPCFG="sslctxid",<httpCtrl->sckt>
            atcmd_invokeReuseLock("AT+QHTTPCFG=\"sslctxid\",%d",  (int)httpCtrl->dataCntxt);
            rslt = atcmd_awaitResult();
            if (rslt != resultCode__success)
            {
                atcmd_close();
                return rslt;
            }
        }

        /* SET URL FOR REQUEST
        * set BGx HTTP URL: AT+QHTTPURL=<urlLength>,timeoutSec  (BGx default timeout is 60, if not specified)
        * wait for CONNECT prompt, then output <URL>, /r/n/r/nOK
        * 
        * NOTE: there is only 1 URL in the BGx at a time
        *---------------------------------------------------------------------------------------------------------------*/

        rslt = S__setUrl(httpCtrl->hostUrl, relativeUrl);
        if (rslt != resultCode__success)
        {
            DPRINT(PRNT_WARN, "Failed set URL rslt=%d\r\n", rslt);
            atcmd_close();
            return rslt;
        }

        /* INVOKE HTTP GET METHOD
        * BGx responds with OK immediately upon acceptance of cmd, then later (up to timeout) with "+QHTTPGET: " string
        * After "OK" we switch IOP to data mode and return. S_httpDoWork() handles the parsing of the page response and
        * if successful, the issue of the AT+QHTTPREAD command to start the page data stream
        * 
        * This allows other application tasks to be performed while waiting for page. No LTEm commands can be invoked
        * but non-LTEm tasks like reading sensors can continue.
        *---------------------------------------------------------------------------------------------------------------*/

        /* If custom headers, need to both set flag and include in request stream below
         */
        if (customRequest == NULL || customRequest->headersLen == 0)
        {
            atcmd_invokeReuseLock("AT+QHTTPCFG=\"requestheader\",1");
            rslt = atcmd_awaitResult();
            if (rslt != resultCode__success)
            {
                atcmd_close();
                return rslt;
            }
            atcmd_configDataMode(httpCtrl->dataCntxt, "CONNECT", atcmd_stdTxDataHndlr, customRequest->requestBuffer, customRequest->headersLen, NULL, true);
            atcmd_invokeReuseLock("AT+QHTTPGET=%d,%d", httpCtrl->timeoutSec, customRequest->headersLen);
        }
        else
        {
            atcmd_invokeReuseLock("AT+QHTTPGET=%d", PERIOD_FROM_SECONDS(httpCtrl->timeoutSec));
        }

        rslt = atcmd_awaitResultWithOptions(PERIOD_FROM_SECONDS(httpCtrl->timeoutSec), S__httpGetStatusParser);                                                                     // wait for "+QHTTPGET trailer (request completed)
        if (rslt == resultCode__success && atcmd_getValue() == 0)
        {
            httpCtrl->httpStatus = S__parseResponseForHttpStatus(httpCtrl, atcmd_getResponse());
            if (httpCtrl->httpStatus >= resultCode__success && httpCtrl->httpStatus <= resultCode__successMax)
            {
                httpCtrl->requestState = httpState_requestComplete;                                         // update httpState, got GET/POST response
                DPRINT(PRNT_MAGENTA, "GetRqst dCntxt:%d, status=%d\r\n", httpCtrl->dataCntxt, httpCtrl->httpStatus);
            }
        }
        else
        {
            httpCtrl->requestState = httpState_idle;
            httpCtrl->httpStatus = atcmd_getValue();
            DPRINT(PRNT_WARN, "Closed failed GET request, status=%d %s\r\n", httpCtrl->httpStatus, atcmd_getErrorDetail());
        }
        atcmd_close();
        return httpCtrl->httpStatus;
    }
    return resultCode__timeout;
}   /* http_get() */


/**
 *	@brief Performs a HTTP POST page web request.
 *  -----------------------------------------------------------------------------------------------
 */
resultCode_t http_post(httpCtrl_t *httpCtrl, const char *relativeUrl, const char *postData, uint16_t postDataSz, bool returnResponseHdrs)
{
    return S__httpPOST(httpCtrl, relativeUrl, NULL, postData, postDataSz, returnResponseHdrs);
}


/**
 *	@brief Performs a HTTP POST page web request.
 *  -----------------------------------------------------------------------------------------------
 */
resultCode_t http_postCustomRequest(httpCtrl_t *httpCtrl, const char* relativeUrl, httpRequest_t* customRequest, bool returnResponseHdrs)
{
    return S__httpPOST(httpCtrl, relativeUrl, customRequest, NULL, 0, returnResponseHdrs);
}


/**
 *	@brief Performs a HTTP POST page web request.
 *  -----------------------------------------------------------------------------------------------
 */
resultCode_t S__httpPOST(httpCtrl_t *httpCtrl, const char *relativeUrl, httpRequest_t* customRequest, const char *postData, uint16_t postDataSz, bool returnResponseHdrs)
{
    httpCtrl->requestState = httpState_idle;
    httpCtrl->httpStatus = resultCode__unknown;
    strcpy(httpCtrl->requestType, "POST");
    resultCode_t rslt;

    if (ATCMD_awaitLock(httpCtrl->timeoutSec))
    {
        if (returnResponseHdrs)
        {
            atcmd_invokeReuseLock("AT+QHTTPCFG=\"responseheader\",%d",  (int)(httpCtrl->returnResponseHdrs));

            rslt = atcmd_awaitResult();
            if (rslt != resultCode__success)
            {
                atcmd_close();
                return rslt;
            }
        }

        if (httpCtrl->useTls)
        {
            // AT+QHTTPCFG="sslctxid",<httpCtrl->sckt>
            atcmd_invokeReuseLock("AT+QHTTPCFG=\"sslctxid\",%d",  (int)httpCtrl->dataCntxt);
            rslt = atcmd_awaitResult();
            if (rslt != resultCode__success)
            {
                atcmd_close();
                return rslt;
            }
        }

        /* SET URL FOR REQUEST
        * set BGx HTTP URL: AT+QHTTPURL=<urlLength>,timeoutSec  (BGx default timeout is 60, if not specified)
        * wait for CONNECT prompt, then output <URL>, /r/n/r/nOK
        * 
        * NOTE: there is only 1 URL in the BGx at a time
        *---------------------------------------------------------------------------------------------------------------*/

        rslt = S__setUrl(httpCtrl->hostUrl, relativeUrl);
        if (rslt != resultCode__success)
        {
            DPRINT(PRNT_WARN, "Failed set URL rslt=%d\r\n", rslt);
            atcmd_close();
            return rslt;
        }

        /* INVOKE HTTP ** POST ** METHOD
        * BGx responds with OK immediately upon acceptance of cmd, then later (up to timeout) with "+QHTTPPOST: " string
        * After "OK" we switch IOP to data mode and return. S_httpDoWork() handles the parsing of the page response and
        * if successful, the issue of the AT+QHTTPREAD command to start the page data stream
        * 
        * This allows other application tasks to be performed while waiting for page. No LTEm commands can be invoked
        * but non-LTEm tasks like reading sensors can continue.
        *---------------------------------------------------------------------------------------------------------------*/
        // atcmd_reset(false);                                                                             // reset atCmd control struct WITHOUT clearing lock

        /* If custom headers, need to both set flag and include in request stream below
         */
        if (customRequest != NULL)
        {
            atcmd_invokeReuseLock("AT+QHTTPCFG=\"requestheader\",1");
            rslt = atcmd_awaitResult();
            if (rslt != resultCode__success)
            {
                atcmd_close();
                return rslt;
            }

            // fixup content-length header value
            char contentLengthVal[5];
            snprintf(contentLengthVal, sizeof(contentLengthVal), "%5d", customRequest->contentLen);
            char* contentLengthPtr = customRequest->requestBuffer + customRequest->headersLen - 9;      // backup over the /r/n/r/n and content-length value
            memcpy(contentLengthPtr, contentLengthVal, 5);

            uint16_t dataLen = customRequest->headersLen + customRequest->contentLen;
            atcmd_configDataMode(httpCtrl->dataCntxt, "CONNECT", atcmd_stdTxDataHndlr, customRequest->requestBuffer, dataLen, NULL, true);
            atcmd_invokeReuseLock("AT+QHTTPPOST=%d,5,%d", strlen(postData), httpCtrl->timeoutSec);
        }
        else
        {
            uint16_t dataLength = strlen(postData);
            atcmd_configDataMode(httpCtrl->dataCntxt, "CONNECT", atcmd_stdTxDataHndlr, postData, dataLength, NULL, true);
            atcmd_invokeReuseLock("AT+QHTTPPOST=%d,5,%d", strlen(postData), httpCtrl->timeoutSec);
        }
        
        rslt = atcmd_awaitResultWithOptions(PERIOD_FROM_SECONDS(httpCtrl->timeoutSec), S__httpPostStatusParser);
        if (rslt == resultCode__success)
        {
            // atcmd_reset(false);                                                                         // clear CONNECT event from atcmd results
            // atcmd_sendCmdData(postData, postDataSz);
            // rslt = atcmd_awaitResultWithOptions(httpCtrl->timeoutSec, S__httpPostStatusParser);
            if (rslt == resultCode__success && atcmd_getValue() == 0)                                   // wait for "+QHTTPPOST trailer: rslt=200, postErr=0
            {
                httpCtrl->httpStatus = S__parseResponseForHttpStatus(httpCtrl, atcmd_getResponse());
                if (httpCtrl->httpStatus >= resultCode__success && httpCtrl->httpStatus <= resultCode__successMax)
                {
                    httpCtrl->requestState = httpState_requestComplete;                                 // update httpState, got GET/POST response
                    DPRINT(PRNT_MAGENTA, "PostRqst dCntxt:%d, status=%d\r\n", httpCtrl->dataCntxt, httpCtrl->httpStatus);
                }
            }
            else
            {
                httpCtrl->requestState = httpState_idle;
                httpCtrl->httpStatus = rslt;
                DPRINT(PRNT_WARN, "Closed failed POST request, status=%d (%s)\r\n", httpCtrl->httpStatus, atcmd_getErrorDetail());
            }
        }
        else
            httpCtrl->httpStatus = resultCode__internalError;
        atcmd_close();
        return httpCtrl->httpStatus;
    }   // awaitLock()

    return resultCode__timeout;
}  // http_post()


/**
 *	@brief Sends contents of a file (LTEM filesystem) as POST to remote.
 */
uint16_t http_postFile(httpCtrl_t *httpCtrl, const char *relativeUrl, const char* filename, bool returnResponseHdrs)
{
    httpCtrl->requestState = httpState_idle;
    httpCtrl->httpStatus = resultCode__unknown;
    strcpy(httpCtrl->requestType, "POST");
    resultCode_t rslt;

    if (ATCMD_awaitLock(httpCtrl->timeoutSec))
    {
        if (returnResponseHdrs)
        {
            atcmd_invokeReuseLock("AT+QHTTPCFG=\"responseheader\",%d",  (int)(httpCtrl->returnResponseHdrs));

            rslt = atcmd_awaitResult();
            if (rslt != resultCode__success)
            {
                atcmd_close();
                return rslt;
            }
        }

        if (httpCtrl->useTls)
        {
            atcmd_invokeReuseLock("AT+QHTTPCFG=\"sslctxid\",%d",  (int)httpCtrl->dataCntxt);
            rslt = atcmd_awaitResult();
            if (rslt != resultCode__success)
            {
                atcmd_close();
                return rslt;
            }
        }

        /* SET URL FOR REQUEST
        * set BGx HTTP URL: AT+QHTTPURL=<urlLength>,timeoutSec  (BGx default timeout is 60, if not specified)
        * wait for CONNECT prompt, then output <URL>, /r/n/r/nOK
        * 
        * NOTE: there is only 1 URL in the BGx at a time
        *---------------------------------------------------------------------------------------------------------------*/

        rslt = S__setUrl(httpCtrl->hostUrl, relativeUrl);
        if (rslt != resultCode__success)
        {
            DPRINT(PRNT_WARN, "Failed set URL rslt=%d\r\n", rslt);
            atcmd_close();
            return rslt;
        }

        /* POST file IS a "custom" request need set flag for custom request/headers
         */
        atcmd_invokeReuseLock("AT+QHTTPCFG=\"requestheader\",1");
        rslt = atcmd_awaitResult();
        if (rslt != resultCode__success)
        {
            atcmd_close();
            return rslt;
        }

        /* INVOKE HTTP ** POST ** METHOD
        * BGx responds with OK immediately upon acceptance of cmd, then later (up to timeout) with "+QHTTPPOST: " string
        * After "OK" we switch IOP to data mode and return. S_httpDoWork() handles the parsing of the page response and
        * if successful, the issue of the AT+QHTTPREAD command to start the page data stream
        * 
        * This allows other application tasks to be performed while waiting for page. No LTEm commands can be invoked
        * but non-LTEm tasks like reading sensors can continue.
        *---------------------------------------------------------------------------------------------------------------*/
        atcmd_reset(false);                                                                             // reset atCmd control struct WITHOUT clearing lock
        atcmd_invokeReuseLock("AT+QHTTPPOSTFILE=\"%s\",15", filename);

        rslt = atcmd_awaitResultWithOptions(PERIOD_FROM_SECONDS(httpCtrl->timeoutSec), S__httpPostFileStatusParser);
        if (rslt == resultCode__success)
        {
            if (rslt == resultCode__success && atcmd_getValue() == 0)                                   // wait for "+QHTTPPOST trailer: rslt=200, postErr=0
            {
                httpCtrl->httpStatus = S__parseResponseForHttpStatus(httpCtrl, atcmd_getResponse());
                if (httpCtrl->httpStatus >= resultCode__success && httpCtrl->httpStatus <= resultCode__successMax)
                {
                    httpCtrl->requestState = httpState_requestComplete;                                 // update httpState, got GET/POST response
                    DPRINT(PRNT_MAGENTA, "Post(file) Request dCntxt:%d, status=%d\r\n", httpCtrl->dataCntxt, httpCtrl->httpStatus);
                }
            }
            else
            {
                httpCtrl->requestState = httpState_idle;
                httpCtrl->httpStatus = rslt;
                DPRINT(PRNT_WARN, "Closed failed POST(file) request, status=%d (%s)\r\n", httpCtrl->httpStatus, atcmd_getErrorDetail());
            }
        }
        else
            httpCtrl->httpStatus = resultCode__internalError;
        atcmd_close();
        return httpCtrl->httpStatus;
    }   // awaitLock()

    return resultCode__timeout;
}  // http_postFile()


/**
 * @brief Retrieves page results from a previous GET or POST.
 * @return HTTP status code from server
 */
uint16_t http_readPage(httpCtrl_t *httpCtrl)
{
    resultCode_t rslt;

    if (httpCtrl->requestState != httpState_requestComplete)
        return resultCode__preConditionFailed;                                  // readPage() only valid after a completed GET\POST
    
    bBuffer_t* rxBffr = g_lqLTEM.iop->rxBffr;                                   // for better readability
    char* workPtr;

    if (atcmd_tryInvoke("AT+QHTTPREAD=%d", httpCtrl->timeoutSec))
    {
        atcmd_configDataMode(httpCtrl->dataCntxt, "CONNECT", S__httpRxHndlr, NULL, 0, httpCtrl->appRecvDataCB, false);
        // atcmd_setStreamControl("CONNECT", (streamCtrl_t*)httpCtrl);
        return atcmd_awaitResult();                                             // dataHandler will be invoked by atcmd module and return a resultCode
    }
    return resultCode__conflict;
}


/**
 * @brief Read HTTP page to BGx file system
 * @return HTTP status code from server
 */
uint16_t http_readPageToFile(httpCtrl_t *httpCtrl, const char* filename)
{
    ASSERT(strlen(filename) < http__readToFileNameSzMax);

    if (httpCtrl->requestState != httpState_requestComplete)
        return resultCode__preConditionFailed;                                  // readPage() only valid after a completed GET\POST
    
    if (atcmd_tryInvoke("AT+QHTTPREADFILE=\"%s\",%d", filename, http__readToFileInterPcktTimeoutSec))
    {
        resultCode_t rslt = atcmd_awaitResultWithOptions(PERIOD_FROM_SECONDS(http__readToFileTimeoutSec), S__httpReadFileStatusParser);
        if (IS_SUCCESS(rslt))
        {
            if (strlen(atcmd_getRawResponse()) > sizeof("AT+QHTTPREADFILE: 0") && *atcmd_getResponse() == '0')
                return resultCode__success;
            else
                return resultCode__internalError;
        }
        return resultCode__extendedBase + rslt;
    }
    return resultCode__conflict;
}


/**
 * @brief Clear state for a request to abandon read
 */
void http_cancelPage(httpCtrl_t *httpCtrl)
{
    ASSERT(false);                  // not implemented
}


#pragma endregion


#pragma region Static Functions
/*-----------------------------------------------------------------------------------------------*/

/**
 * @brief Helper function to create a URL from host and relative parts.
 */
static resultCode_t S__setUrl(const char *host, const char *relative)
{
    uint16_t rslt;
    bool urlSet = false;
    char url[240] = {0};
    
    strcpy(url, host);
    if (strlen(relative) > 0)                                                                       // need to concat relative/query
    {
        strcat(url, relative);
    }
    DPRINT(PRNT_dMAGENTA, "URL(%d)=%s", strlen(url), url);
    DPRINT(PRNT_dMAGENTA, "\r\n");                                                                  // separate line-end if URL truncates in trace
    
    atcmd_configDataMode(0, "CONNECT\r\n", atcmd_stdTxDataHndlr, url, strlen(url), NULL, false);    // setup for URL dataMode transfer 
    atcmd_invokeReuseLock("AT+QHTTPURL=%d,5", strlen(url));
    rslt = atcmd_awaitResult();
    return rslt;
}


// /**
//  * @brief Set the content length header, if customHeaders and not already set.
//  * @note Requires 25 char of free space in request headers buffer
//  * 
//  * @param requestHeaders Char buffer holding the custom request headers
//  * @param requestHeadersSz Size of the buffer
//  * @param contentLength Size of content; 0 for GET requests, strlen(postData) for POST requests
//  */
// static void S__setContentLength(char requestHeaders, uint16_t requestHeadersSz, uint16_t contentLength)
// {
//     ASSERT(strlen(requestHeaders) + 25 < requestHeadersSz);

//     if (strstr(requestHeaders, "Content-Length: ") == NULL)                                         // already present
//     {
//         char workBffr[80];
//         snprintf(workBffr, sizeof(workBffr), "Content-Length: %d\r\n\r\n", contentLength);
//         strcat(requestHeaders, workBffr);
//     }
// }


/**
 * @brief Once the result is obtained, this function extracts the HTTP status value from the response
 */
static uint16_t S__parseResponseForHttpStatus(httpCtrl_t *httpCtrl, const char *response)
{
    char *continueAt = strchr(response, ',');                               // skip ',' and parse http status
    if (continueAt)
    {
        httpCtrl->httpStatus = strtol(++continueAt, &continueAt, 10);
        httpCtrl->pageSize = strtol(++continueAt, &continueAt, 10);         // skip next ',' and parse content length

        httpCtrl->pageRemaining = 0;
        if (httpCtrl->pageSize > 0)
            httpCtrl->pageRemaining = httpCtrl->pageSize;                   // read() will decrement this
    }
    else
        httpCtrl->httpStatus = resultCode__preConditionFailed;
    return httpCtrl->httpStatus;
}

/**
 * @brief Handles the READ data flow from the BGx (via rxBffr) to app
 */
static resultCode_t S__httpRxHndlr()
{
    char wrkBffr[32];
    uint16_t pageRslt = 0;

    httpCtrl_t *httpCtrl = (httpCtrl_t*)ltem_getStreamFromCntxt(g_lqLTEM.atcmd->dataMode.contextKey, streamType_HTTP);
    ASSERT(httpCtrl != NULL);                                                                           // ASSERT data mode and stream context are consistent

    uint8_t popCnt = bbffr_find(g_lqLTEM.iop->rxBffr, "\r", 0, 0, false);
    if (BBFFR_ISNOTFOUND(popCnt))
    {
        return resultCode__internalError;
    }
    
    bbffr_pop(g_lqLTEM.iop->rxBffr, wrkBffr, popCnt + 2);                                               // pop CONNECT phrase for parsing data length
    DPRINT(PRNT_CYAN, "httpPageRcvr() stream started\r\n");

    memset(wrkBffr, 0, sizeof(wrkBffr));                                                                // need clean wrkBffr for trailer parsing
    uint32_t readStart = pMillis();
    do
    {
        uint16_t occupiedCnt = bbffr_getOccupied(g_lqLTEM.iop->rxBffr);
        bool readTimeout = pMillis() - readStart > httpCtrl->timeoutSec;
        uint16_t trailerIndx = bbffr_find(g_lqLTEM.iop->rxBffr, "\r\nOK\r\n\r\n", 0, 0, false);
        uint16_t reqstBlockSz = MIN(trailerIndx, httpCtrl->defaultBlockSz);

        if (bbffr_getOccupied(g_lqLTEM.iop->rxBffr) >= reqstBlockSz)                                        // sufficient read content ready
        {
            char* streamPtr;
            uint16_t blockSz = bbffr_popBlock(g_lqLTEM.iop->rxBffr, &streamPtr, reqstBlockSz);              // get address from rxBffr
            DPRINT(PRNT_CYAN, "httpPageRcvr() ptr=%p blkSz=%d isFinal=%d\r\n", streamPtr, blockSz, BBFFR_ISFOUND(trailerIndx));

            // forward to application
            ((httpRecv_func)(*httpCtrl->appRecvDataCB))(httpCtrl->dataCntxt, streamPtr, blockSz, BBFFR_ISFOUND(trailerIndx));
            bbffr_popBlockFinalize(g_lqLTEM.iop->rxBffr, true);                                             // commit POP
        }

        if (BBFFR_ISFOUND(trailerIndx))
        {
            // parse trailer for status 
            uint8_t offset = strlen(wrkBffr);
            bbffr_pop(g_lqLTEM.iop->rxBffr, wrkBffr + offset, sizeof(wrkBffr) - offset);

            if (strchr(wrkBffr, '\n'))                                                                      // wait for final /r/n in wrkBffr
            {
                char* suffix = strstr(wrkBffr, "+QHTTPREAD: ") + sizeof("+QHTTPREAD: ");
                uint16_t errVal = strtol(suffix, NULL, 10);
                if (errVal == 0)
                {
                    return resultCode__success;
                }
                else
                {
                    pageRslt = errVal;
                    // to be translated like file results
                    return pageRslt;
                }
            }
        }
    } while (true);
}

#pragma endregion


#pragma Static Response Parsers

/* Note for parsers below:
 * httprspcode is only reported if err is 0, have to search for finale (\r\n) after a preamble (parser speak)
 * --------------------------------------------------------------------------------------------- */

static cmdParseRslt_t S__httpGetStatusParser() 
{
    // +QHTTPGET: <err>[,<httprspcode>[,<content_length>]]
    return atcmd_stdResponseParser("+QHTTPGET: ", true, ",", 0, 1, "\r\n", 0);
}


static cmdParseRslt_t S__httpPostStatusParser() 
{
    // +QHTTPPOST: <err>[,<httprspcode>[,<content_length>]] 
    return atcmd_stdResponseParser("+QHTTPPOST: ", true, ",", 0, 1, "\r\n", 0);
}


static cmdParseRslt_t S__httpReadFileStatusParser() 
{
    // +QHTTPPOST: <err>[,<httprspcode>[,<content_length>]] 
    return atcmd_stdResponseParser("+QHTTPREADFILE: ", true, ",", 0, 1, "\r\n", 0);
}


static cmdParseRslt_t S__httpPostFileStatusParser() 
{
    // +QHTTPPOST: <err>[,<httprspcode>[,<content_length>]] 
    return atcmd_stdResponseParser("+QHTTPPOSTFILE: ", true, ",", 0, 1, "\r\n", 0);
}

#pragma endregion