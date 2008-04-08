/*
 * Parser (Base for parsers and splitters)
 *
 * Copyright 2003 Robert Shearman
 * Copyright 2004-2005 Christian Costa
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "quartz_private.h"
#include "control_private.h"
#include "pin.h"

#include "vfwmsgs.h"
#include "amvideo.h"

#include "wine/unicode.h"
#include "wine/debug.h"

#include <math.h>
#include <assert.h>

#include "parser.h"

WINE_DEFAULT_DEBUG_CHANNEL(quartz);

static const WCHAR wcsInputPinName[] = {'i','n','p','u','t',' ','p','i','n',0};
static const IBaseFilterVtbl Parser_Vtbl;
static const IMediaSeekingVtbl Parser_Seeking_Vtbl;
static const IPinVtbl Parser_OutputPin_Vtbl;
static const IPinVtbl Parser_InputPin_Vtbl;

static HRESULT Parser_OutputPin_QueryAccept(LPVOID iface, const AM_MEDIA_TYPE * pmt);
static HRESULT Parser_ChangeCurrent(IBaseFilter *iface);
static HRESULT Parser_ChangeStop(IBaseFilter *iface);
static HRESULT Parser_ChangeRate(IBaseFilter *iface);

static inline ParserImpl *impl_from_IMediaSeeking( IMediaSeeking *iface )
{
    return (ParserImpl *)((char*)iface - FIELD_OFFSET(ParserImpl, mediaSeeking.lpVtbl));
}


HRESULT Parser_Create(ParserImpl* pParser, const CLSID* pClsid, PFN_PROCESS_SAMPLE fnProcessSample, PFN_QUERY_ACCEPT fnQueryAccept, PFN_PRE_CONNECT fnPreConnect, PFN_CLEANUP fnCleanup, CHANGEPROC stop, CHANGEPROC current, CHANGEPROC rate)
{
    HRESULT hr;
    PIN_INFO piInput;

    /* pTransformFilter is already allocated */
    pParser->clsid = *pClsid;

    pParser->lpVtbl = &Parser_Vtbl;
    pParser->refCount = 1;
    InitializeCriticalSection(&pParser->csFilter);
    pParser->csFilter.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": ParserImpl.csFilter");
    pParser->state = State_Stopped;
    pParser->pClock = NULL;
    pParser->fnCleanup = fnCleanup;
    ZeroMemory(&pParser->filterInfo, sizeof(FILTER_INFO));

    pParser->cStreams = 0;
    pParser->ppPins = CoTaskMemAlloc(1 * sizeof(IPin *));

    /* construct input pin */
    piInput.dir = PINDIR_INPUT;
    piInput.pFilter = (IBaseFilter *)pParser;
    lstrcpynW(piInput.achName, wcsInputPinName, sizeof(piInput.achName) / sizeof(piInput.achName[0]));

    if (!current)
        current = Parser_ChangeCurrent;

    if (!stop)
        stop = Parser_ChangeStop;

    if (!rate)
        rate = Parser_ChangeRate;

    MediaSeekingImpl_Init((IBaseFilter*)pParser, stop, current, rate, &pParser->mediaSeeking, &pParser->csFilter);
    pParser->mediaSeeking.lpVtbl = &Parser_Seeking_Vtbl;

    hr = PullPin_Construct(&Parser_InputPin_Vtbl, &piInput, fnProcessSample, (LPVOID)pParser, fnQueryAccept, fnCleanup, &pParser->csFilter, (IPin **)&pParser->pInputPin);

    if (SUCCEEDED(hr))
    {
        pParser->ppPins[0] = (IPin *)pParser->pInputPin;
        pParser->pInputPin->fnPreConnect = fnPreConnect;
    }
    else
    {
        CoTaskMemFree(pParser->ppPins);
        pParser->csFilter.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection(&pParser->csFilter);
        CoTaskMemFree(pParser);
    }

    return hr;
}

static HRESULT WINAPI Parser_QueryInterface(IBaseFilter * iface, REFIID riid, LPVOID * ppv)
{
    ParserImpl *This = (ParserImpl *)iface;
    TRACE("(%s, %p)\n", qzdebugstr_guid(riid), ppv);

    *ppv = NULL;

    if (IsEqualIID(riid, &IID_IUnknown))
        *ppv = (LPVOID)This;
    else if (IsEqualIID(riid, &IID_IPersist))
        *ppv = (LPVOID)This;
    else if (IsEqualIID(riid, &IID_IMediaFilter))
        *ppv = (LPVOID)This;
    else if (IsEqualIID(riid, &IID_IBaseFilter))
        *ppv = (LPVOID)This;
    else if (IsEqualIID(riid, &IID_IMediaSeeking))
        *ppv = (LPVOID)&This->mediaSeeking;

    if (*ppv)
    {
        IUnknown_AddRef((IUnknown *)(*ppv));
        return S_OK;
    }

    if (!IsEqualIID(riid, &IID_IPin))
        FIXME("No interface for %s!\n", qzdebugstr_guid(riid));

    return E_NOINTERFACE;
}

static ULONG WINAPI Parser_AddRef(IBaseFilter * iface)
{
    ParserImpl *This = (ParserImpl *)iface;
    ULONG refCount = InterlockedIncrement(&This->refCount);

    TRACE("(%p/%p)->() AddRef from %d\n", This, iface, refCount - 1);

    return refCount;
}

static ULONG WINAPI Parser_Release(IBaseFilter * iface)
{
    ParserImpl *This = (ParserImpl *)iface;
    ULONG refCount = InterlockedDecrement(&This->refCount);

    TRACE("(%p)->() Release from %d\n", This, refCount + 1);
    
    if (!refCount)
    {
        ULONG i;

        if (This->fnCleanup)
            This->fnCleanup(This);

        if (This->pClock)
            IReferenceClock_Release(This->pClock);
        
        for (i = 0; i < This->cStreams + 1; i++)
        {
            IPin *pConnectedTo;

            if (SUCCEEDED(IPin_ConnectedTo(This->ppPins[i], &pConnectedTo)))
            {
                IPin_Disconnect(pConnectedTo);
                IPin_Release(pConnectedTo);
            }
            IPin_Disconnect(This->ppPins[i]);

            IPin_Release(This->ppPins[i]);
        }
        
        CoTaskMemFree(This->ppPins);
        This->lpVtbl = NULL;

        This->csFilter.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection(&This->csFilter);
        
        TRACE("Destroying parser\n");
        CoTaskMemFree(This);
        
        return 0;
    }
    else
        return refCount;
}

/** IPersist methods **/

static HRESULT WINAPI Parser_GetClassID(IBaseFilter * iface, CLSID * pClsid)
{
    ParserImpl *This = (ParserImpl *)iface;

    TRACE("(%p)\n", pClsid);

    *pClsid = This->clsid;

    return S_OK;
}

/** IMediaFilter methods **/

static HRESULT WINAPI Parser_Stop(IBaseFilter * iface)
{
    HRESULT hr;
    ParserImpl *This = (ParserImpl *)iface;
    PullPin *pin = (PullPin *)This->ppPins[0];

    TRACE("()\n");

    EnterCriticalSection(&pin->thread_lock);
    EnterCriticalSection(&This->csFilter);
    {
        if (This->state == State_Stopped)
        {
            LeaveCriticalSection(&This->csFilter);
            LeaveCriticalSection(&pin->thread_lock);
            return S_OK;
        }
        This->state = State_Stopped;
    }
    LeaveCriticalSection(&This->csFilter);

    hr = PullPin_StopProcessing(This->pInputPin);
    LeaveCriticalSection(&pin->thread_lock);
    return hr;
}

static HRESULT WINAPI Parser_Pause(IBaseFilter * iface)
{
    HRESULT hr = S_OK;
    ParserImpl *This = (ParserImpl *)iface;
    PullPin *pin = (PullPin *)This->ppPins[0];

    TRACE("()\n");

    EnterCriticalSection(&pin->thread_lock);
    EnterCriticalSection(&This->csFilter);

    if (This->state == State_Paused)
    {
        LeaveCriticalSection(&This->csFilter);
        LeaveCriticalSection(&pin->thread_lock);
        return S_OK;
    }

    if (This->state == State_Stopped)
    {
        LeaveCriticalSection(&This->csFilter);
        hr = IBaseFilter_Run(iface, -1);
        EnterCriticalSection(&This->csFilter);
    }

    This->state = State_Paused;

    LeaveCriticalSection(&This->csFilter);
    if (SUCCEEDED(hr))
        hr = PullPin_PauseProcessing(This->pInputPin);
    LeaveCriticalSection(&pin->thread_lock);

    return hr;
}

static HRESULT WINAPI Parser_Run(IBaseFilter * iface, REFERENCE_TIME tStart)
{
    HRESULT hr = S_OK;
    ParserImpl *This = (ParserImpl *)iface;
    PullPin *pin = (PullPin *)This->ppPins[0];

    int i;

    TRACE("(%s)\n", wine_dbgstr_longlong(tStart));

    EnterCriticalSection(&pin->thread_lock);
    EnterCriticalSection(&This->csFilter);
    {
        if (This->state == State_Running)
        {
            LeaveCriticalSection(&This->csFilter);
            LeaveCriticalSection(&pin->thread_lock);
            return S_OK;
        }

        This->rtStreamStart = tStart;

        if (SUCCEEDED(hr) && (This->state == State_Stopped))
        {
            LeaveCriticalSection(&This->csFilter);
            hr = PullPin_InitProcessing(This->pInputPin);
            EnterCriticalSection(&This->csFilter);

            if (SUCCEEDED(hr))
            { 
                for (i = 1; i < (This->cStreams + 1); i++)
                {
                    OutputPin_CommitAllocator((OutputPin *)This->ppPins[i]);
                }
            }
        }

        if (SUCCEEDED(hr))
        {
            LeaveCriticalSection(&This->csFilter);
            hr = PullPin_StartProcessing(This->pInputPin);
            EnterCriticalSection(&This->csFilter);
        }

        if (SUCCEEDED(hr))
            This->state = State_Running;
    }
    LeaveCriticalSection(&This->csFilter);
    LeaveCriticalSection(&pin->thread_lock);

    return hr;
}

static HRESULT WINAPI Parser_GetState(IBaseFilter * iface, DWORD dwMilliSecsTimeout, FILTER_STATE *pState)
{
    ParserImpl *This = (ParserImpl *)iface;
    PullPin *pin = (PullPin *)This->ppPins[0];
    HRESULT hr = S_OK;

    TRACE("(%d, %p)\n", dwMilliSecsTimeout, pState);

    EnterCriticalSection(&pin->thread_lock);
    EnterCriticalSection(&This->csFilter);
    {
        *pState = This->state;
    }
    LeaveCriticalSection(&This->csFilter);

    if (This->pInputPin && (PullPin_WaitForStateChange(This->pInputPin, dwMilliSecsTimeout) == S_FALSE))
        hr = VFW_S_STATE_INTERMEDIATE;
    LeaveCriticalSection(&pin->thread_lock);

    return hr;
}

static HRESULT WINAPI Parser_SetSyncSource(IBaseFilter * iface, IReferenceClock *pClock)
{
    ParserImpl *This = (ParserImpl *)iface;
    PullPin *pin = (PullPin *)This->ppPins[0];

    TRACE("(%p)\n", pClock);

    EnterCriticalSection(&pin->thread_lock);
    EnterCriticalSection(&This->csFilter);
    {
        if (This->pClock)
            IReferenceClock_Release(This->pClock);
        This->pClock = pClock;
        if (This->pClock)
            IReferenceClock_AddRef(This->pClock);
    }
    LeaveCriticalSection(&This->csFilter);
    LeaveCriticalSection(&pin->thread_lock);

    return S_OK;
}

static HRESULT WINAPI Parser_GetSyncSource(IBaseFilter * iface, IReferenceClock **ppClock)
{
    ParserImpl *This = (ParserImpl *)iface;

    TRACE("(%p)\n", ppClock);

    EnterCriticalSection(&This->csFilter);
    {
        *ppClock = This->pClock;
        if (This->pClock)
            IReferenceClock_AddRef(This->pClock);
    }
    LeaveCriticalSection(&This->csFilter);
    
    return S_OK;
}

/** IBaseFilter implementation **/

static HRESULT WINAPI Parser_EnumPins(IBaseFilter * iface, IEnumPins **ppEnum)
{
    ENUMPINDETAILS epd;
    ParserImpl *This = (ParserImpl *)iface;

    TRACE("(%p)\n", ppEnum);

    epd.cPins = This->cStreams + 1; /* +1 for input pin */
    epd.ppPins = This->ppPins;
    return IEnumPinsImpl_Construct(&epd, ppEnum);
}

static HRESULT WINAPI Parser_FindPin(IBaseFilter * iface, LPCWSTR Id, IPin **ppPin)
{
    FIXME("(%p)->(%s,%p)\n", iface, debugstr_w(Id), ppPin);

    /* FIXME: critical section */

    return E_NOTIMPL;
}

static HRESULT WINAPI Parser_QueryFilterInfo(IBaseFilter * iface, FILTER_INFO *pInfo)
{
    ParserImpl *This = (ParserImpl *)iface;

    TRACE("(%p)\n", pInfo);

    strcpyW(pInfo->achName, This->filterInfo.achName);
    pInfo->pGraph = This->filterInfo.pGraph;

    if (pInfo->pGraph)
        IFilterGraph_AddRef(pInfo->pGraph);
    
    return S_OK;
}

static HRESULT WINAPI Parser_JoinFilterGraph(IBaseFilter * iface, IFilterGraph *pGraph, LPCWSTR pName)
{
    HRESULT hr = S_OK;
    ParserImpl *This = (ParserImpl *)iface;

    TRACE("(%p, %s)\n", pGraph, debugstr_w(pName));

    EnterCriticalSection(&This->csFilter);
    {
        if (pName)
            strcpyW(This->filterInfo.achName, pName);
        else
            *This->filterInfo.achName = '\0';
        This->filterInfo.pGraph = pGraph; /* NOTE: do NOT increase ref. count */
    }
    LeaveCriticalSection(&This->csFilter);

    return hr;
}

static HRESULT WINAPI Parser_QueryVendorInfo(IBaseFilter * iface, LPWSTR *pVendorInfo)
{
    TRACE("(%p)\n", pVendorInfo);
    return E_NOTIMPL;
}

static const IBaseFilterVtbl Parser_Vtbl =
{
    Parser_QueryInterface,
    Parser_AddRef,
    Parser_Release,
    Parser_GetClassID,
    Parser_Stop,
    Parser_Pause,
    Parser_Run,
    Parser_GetState,
    Parser_SetSyncSource,
    Parser_GetSyncSource,
    Parser_EnumPins,
    Parser_FindPin,
    Parser_QueryFilterInfo,
    Parser_JoinFilterGraph,
    Parser_QueryVendorInfo
};

HRESULT Parser_AddPin(ParserImpl * This, const PIN_INFO * piOutput, ALLOCATOR_PROPERTIES * props, const AM_MEDIA_TYPE * amt)
{
    IPin ** ppOldPins;
    HRESULT hr;

    ppOldPins = This->ppPins;

    This->ppPins = CoTaskMemAlloc((This->cStreams + 2) * sizeof(IPin *));
    memcpy(This->ppPins, ppOldPins, (This->cStreams + 1) * sizeof(IPin *));

    hr = OutputPin_Construct(&Parser_OutputPin_Vtbl, sizeof(Parser_OutputPin), piOutput, props, NULL, Parser_OutputPin_QueryAccept, &This->csFilter, This->ppPins + This->cStreams + 1);

    if (SUCCEEDED(hr))
    {
        IPin *pPin = This->ppPins[This->cStreams + 1];
        Parser_OutputPin *pin = (Parser_OutputPin *)pPin;
        pin->pmt = CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
        CopyMediaType(pin->pmt, amt);
        pin->dwSamplesProcessed = 0;

        pin->pin.pin.pUserData = (LPVOID)This->ppPins[This->cStreams + 1];
        This->cStreams++;
        CoTaskMemFree(ppOldPins);
    }
    else
    {
        CoTaskMemFree(This->ppPins);
        This->ppPins = ppOldPins;
        ERR("Failed with error %x\n", hr);
    }

    return hr;
}

static HRESULT Parser_RemoveOutputPins(ParserImpl * This)
{
    /* NOTE: should be in critical section when calling this function */

    ULONG i;
    IPin ** ppOldPins = This->ppPins;

    /* reduce the pin array down to 1 (just our input pin) */
    This->ppPins = CoTaskMemAlloc(sizeof(IPin *) * 1);
    memcpy(This->ppPins, ppOldPins, sizeof(IPin *) * 1);

    for (i = 0; i < This->cStreams; i++)
    {
        OutputPin_DeliverDisconnect((OutputPin *)ppOldPins[i + 1]);
        IPin_Release(ppOldPins[i + 1]);
    }

    This->cStreams = 0;
    CoTaskMemFree(ppOldPins);

    return S_OK;
}

static HRESULT Parser_ChangeCurrent(IBaseFilter *iface)
{
    FIXME("(%p) filter hasn't implemented current position change!\n", iface);
    return S_OK;
}

static HRESULT Parser_ChangeStop(IBaseFilter *iface)
{
    FIXME("(%p) filter hasn't implemented stop position change!\n", iface);
    return S_OK;
}

static HRESULT Parser_ChangeRate(IBaseFilter *iface)
{
    FIXME("(%p) filter hasn't implemented rate change!\n", iface);
    return S_OK;
}


static HRESULT WINAPI Parser_Seeking_QueryInterface(IMediaSeeking * iface, REFIID riid, LPVOID * ppv)
{
    ParserImpl *This = impl_from_IMediaSeeking(iface);

    return IUnknown_QueryInterface((IUnknown *)This, riid, ppv);
}

static ULONG WINAPI Parser_Seeking_AddRef(IMediaSeeking * iface)
{
    ParserImpl *This = impl_from_IMediaSeeking(iface);

    return IUnknown_AddRef((IUnknown *)This);
}

static ULONG WINAPI Parser_Seeking_Release(IMediaSeeking * iface)
{
    ParserImpl *This = impl_from_IMediaSeeking(iface);

    return IUnknown_Release((IUnknown *)This);
}

static const IMediaSeekingVtbl Parser_Seeking_Vtbl =
{
    Parser_Seeking_QueryInterface,
    Parser_Seeking_AddRef,
    Parser_Seeking_Release,
    MediaSeekingImpl_GetCapabilities,
    MediaSeekingImpl_CheckCapabilities,
    MediaSeekingImpl_IsFormatSupported,
    MediaSeekingImpl_QueryPreferredFormat,
    MediaSeekingImpl_GetTimeFormat,
    MediaSeekingImpl_IsUsingTimeFormat,
    MediaSeekingImpl_SetTimeFormat,
    MediaSeekingImpl_GetDuration,
    MediaSeekingImpl_GetStopPosition,
    MediaSeekingImpl_GetCurrentPosition,
    MediaSeekingImpl_ConvertTimeFormat,
    MediaSeekingImpl_SetPositions,
    MediaSeekingImpl_GetPositions,
    MediaSeekingImpl_GetAvailable,
    MediaSeekingImpl_SetRate,
    MediaSeekingImpl_GetRate,
    MediaSeekingImpl_GetPreroll
};

static HRESULT WINAPI Parser_OutputPin_QueryInterface(IPin * iface, REFIID riid, LPVOID * ppv)
{
    Parser_OutputPin *This = (Parser_OutputPin *)iface;

    TRACE("(%s, %p)\n", qzdebugstr_guid(riid), ppv);

    *ppv = NULL;

    if (IsEqualIID(riid, &IID_IUnknown))
        *ppv = (LPVOID)iface;
    else if (IsEqualIID(riid, &IID_IPin))
        *ppv = (LPVOID)iface;
    else if (IsEqualIID(riid, &IID_IMediaSeeking))
    {
        return IBaseFilter_QueryInterface(This->pin.pin.pinInfo.pFilter, &IID_IMediaSeeking, ppv);
    }

    if (*ppv)
    {
        IUnknown_AddRef((IUnknown *)(*ppv));
        return S_OK;
    }

    FIXME("No interface for %s!\n", qzdebugstr_guid(riid));

    return E_NOINTERFACE;
}

static ULONG WINAPI Parser_OutputPin_Release(IPin * iface)
{
    Parser_OutputPin *This = (Parser_OutputPin *)iface;
    ULONG refCount = InterlockedDecrement(&This->pin.pin.refCount);
    
    TRACE("(%p)->() Release from %d\n", iface, refCount + 1);
    
    if (!refCount)
    {
        FreeMediaType(This->pmt);
        CoTaskMemFree(This->pmt);
        FreeMediaType(&This->pin.pin.mtCurrent);
        CoTaskMemFree(This);
        return 0;
    }
    return refCount;
}

static HRESULT WINAPI Parser_OutputPin_EnumMediaTypes(IPin * iface, IEnumMediaTypes ** ppEnum)
{
    ENUMMEDIADETAILS emd;
    Parser_OutputPin *This = (Parser_OutputPin *)iface;

    TRACE("(%p)\n", ppEnum);

    /* override this method to allow enumeration of your types */
    emd.cMediaTypes = 1;
    emd.pMediaTypes = This->pmt;

    return IEnumMediaTypesImpl_Construct(&emd, ppEnum);
}

static HRESULT Parser_OutputPin_QueryAccept(LPVOID iface, const AM_MEDIA_TYPE * pmt)
{
    Parser_OutputPin *This = (Parser_OutputPin *)iface;

    TRACE("()\n");
    dump_AM_MEDIA_TYPE(pmt);

    return (memcmp(This->pmt, pmt, sizeof(AM_MEDIA_TYPE)) == 0);
}

static const IPinVtbl Parser_OutputPin_Vtbl = 
{
    Parser_OutputPin_QueryInterface,
    IPinImpl_AddRef,
    Parser_OutputPin_Release,
    OutputPin_Connect,
    OutputPin_ReceiveConnection,
    OutputPin_Disconnect,
    IPinImpl_ConnectedTo,
    IPinImpl_ConnectionMediaType,
    IPinImpl_QueryPinInfo,
    IPinImpl_QueryDirection,
    IPinImpl_QueryId,
    IPinImpl_QueryAccept,
    Parser_OutputPin_EnumMediaTypes,
    IPinImpl_QueryInternalConnections,
    OutputPin_EndOfStream,
    OutputPin_BeginFlush,
    OutputPin_EndFlush,
    OutputPin_NewSegment
};

static HRESULT WINAPI Parser_InputPin_Disconnect(IPin * iface)
{
    HRESULT hr;
    IPinImpl *This = (IPinImpl *)iface;

    TRACE("()\n");

    EnterCriticalSection(This->pCritSec);
    {
        if (This->pConnectedTo)
        {
            FILTER_STATE state;

            hr = IBaseFilter_GetState(This->pinInfo.pFilter, 0, &state);

            if (SUCCEEDED(hr) && (state == State_Stopped))
            {
                IPin_Release(This->pConnectedTo);
                This->pConnectedTo = NULL;
                hr = Parser_RemoveOutputPins((ParserImpl *)This->pinInfo.pFilter);
            }
            else
                hr = VFW_E_NOT_STOPPED;
        }
        else
            hr = S_FALSE;
    }
    LeaveCriticalSection(This->pCritSec);
    
    return hr;
}

HRESULT WINAPI Parser_PullPin_ReceiveConnection(IPin * iface, IPin * pReceivePin, const AM_MEDIA_TYPE * pmt)
{
    HRESULT hr;

    TRACE("()\n");

    hr = PullPin_ReceiveConnection(iface, pReceivePin, pmt);
    if (FAILED(hr))
    {
        IPinImpl *This = (IPinImpl *)iface;

        EnterCriticalSection(This->pCritSec);
        Parser_RemoveOutputPins((ParserImpl *)This->pinInfo.pFilter);
        LeaveCriticalSection(This->pCritSec);
    }

    return hr;
}

static const IPinVtbl Parser_InputPin_Vtbl =
{
    PullPin_QueryInterface,
    IPinImpl_AddRef,
    PullPin_Release,
    OutputPin_Connect,
    Parser_PullPin_ReceiveConnection,
    Parser_InputPin_Disconnect,
    IPinImpl_ConnectedTo,
    IPinImpl_ConnectionMediaType,
    IPinImpl_QueryPinInfo,
    IPinImpl_QueryDirection,
    IPinImpl_QueryId,
    IPinImpl_QueryAccept,
    IPinImpl_EnumMediaTypes,
    IPinImpl_QueryInternalConnections,
    PullPin_EndOfStream,
    PullPin_BeginFlush,
    PullPin_EndFlush,
    PullPin_NewSegment
};
