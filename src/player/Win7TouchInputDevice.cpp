//
//  libavg - Media Playback Engine. 
//  Copyright (C) 2003-2011 Ulrich von Zadow
//
//  This library is free software; you can redistribute it and/or
//  modify it under the terms of the GNU Lesser General Public
//  License as published by the Free Software Foundation; either
//  version 2 of the License, or (at your option) any later version.
//
//  This library is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
//  Lesser General Public License for more details.
//
//  You should have received a copy of the GNU Lesser General Public
//  License along with this library; if not, write to the Free Software
//  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//
//  Current versions can be found at www.libavg.de
//

#include "Win7TouchInputDevice.h"
#include "TouchEvent.h"
#include "Player.h"
#include "AVGNode.h"
#include "TouchStatus.h"
#include "TouchEvent.h"
#include "DisplayEngine.h"

#include "../base/Logger.h"
#include "../base/ObjectCounter.h"
#include "../base/Exception.h"

using namespace std;

namespace avg {

Win7TouchInputDevice* Win7TouchInputDevice::s_pInstance(0);

Win7TouchInputDevice::Win7TouchInputDevice()
    : m_LastID(0)
{
    s_pInstance = this;
}

Win7TouchInputDevice::~Win7TouchInputDevice()
{
    SetWindowLong(m_Hwnd, GWL_WNDPROC, (LONG)m_OldWndProc);
    s_pInstance = 0;
}

typedef bool (WINAPI* PRTWPROC)(HWND, ULONG);

void Win7TouchInputDevice::start()
{
#ifdef SM_DIGITIZER
    AVG_TRACE(Logger::category::CONFIG, Logger::severity::INFO,
            "Using Windows 7 Touch driver.");
    // We need to do runtime dynamic linking for the Win7 touch functions because they 
    // aren't present on pre-Win7 systems.
    HMODULE hUser32 = GetModuleHandle(TEXT("user32.dll"));
    PRTWPROC pRegisterTouchWindowProc = (PRTWPROC) GetProcAddress(hUser32, 
            "RegisterTouchWindow");
    if (!pRegisterTouchWindowProc) {
        throw Exception(AVG_ERR_UNSUPPORTED, 
                "This version of windows does not support Multitouch input.");
    }
    m_pGetTouchInputInfoProc = (GTIIPROC) GetProcAddress(hUser32, "GetTouchInputInfo");
    m_pCloseTouchInputHandleProc = (CTIHPROC) GetProcAddress(hUser32, 
            "CloseTouchInputHandle");

    int multitouchCaps = GetSystemMetrics(SM_DIGITIZER);
    if (multitouchCaps & NID_MULTI_INPUT) {
        MultitouchInputDevice::start();
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        int err = SDL_GetWMInfo(&info);
        AVG_ASSERT(err == 1);
        m_Hwnd = info.window;
        bool bOk = pRegisterTouchWindowProc(m_Hwnd, TWF_FINETOUCH | TWF_WANTPALM);
        AVG_ASSERT(bOk);
        m_OldWndProc = (WNDPROC)SetWindowLong(m_Hwnd, GWL_WNDPROC, (LONG)touchWndSubclassProc);
        m_ClientAreaOffset = calcClientAreaOffset();
    } else {
        throw Exception(AVG_ERR_UNSUPPORTED, "No windows 7 multitouch device connected.");
    }
#else
    throw Exception(AVG_ERR_UNSUPPORTED, 
            "Windows multitouch not supported by this version of libavg.");
#endif
}

#define MOUSEEVENTF_FROMTOUCH 0xFF515700

LRESULT APIENTRY Win7TouchInputDevice::touchWndSubclassProc(HWND hwnd, UINT uMsg,
        WPARAM wParam, LPARAM lParam)
{
#ifdef SM_DIGITIZER
    Win7TouchInputDevice * pThis = Win7TouchInputDevice::s_pInstance;
    bool bIgnore = false;
    switch(uMsg) {
        case WM_TOUCH:
            pThis->onTouch(hwnd, wParam, lParam);
            break;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
            if (GetMessageExtraInfo() & MOUSEEVENTF_FROMTOUCH) { 
                // Click was generated by wisptis / Windows Touch
                bIgnore = true;
            }
            break;
        case WM_MOUSEMOVE:
            // Workaround for Win7 bug: There is no way to figure out if a WM_MOUSEMOVE
            // originates from a touch, so we ignore all mousemove events as long as
            // something is touching the screen.
            bIgnore = (pThis->getNumTouches() > 0);
            break;
        default:
            break;
    } 
    if (!bIgnore) {
        return CallWindowProc(pThis->m_OldWndProc, hwnd, uMsg, wParam, lParam); 
    } else {
        return 0;
    }
#else
    return 0;
#endif
} 

void Win7TouchInputDevice::onTouch(HWND hWnd, WPARAM wParam, LPARAM lParam)
{
#ifdef SM_DIGITIZER
    unsigned numInputs = LOWORD(wParam);
    PTOUCHINPUT pInputs = new TOUCHINPUT[numInputs];
    BOOL bOk = m_pGetTouchInputInfoProc((HTOUCHINPUT)lParam, numInputs, pInputs, 
            sizeof(TOUCHINPUT));
    AVG_ASSERT(bOk);
    RECT winRect;
    bOk = GetWindowRect(m_Hwnd, &winRect);
    AVG_ASSERT(bOk);
    for (unsigned i = 0; i < numInputs; i++) {
        TOUCHINPUT *pTouchInput = &(pInputs[i]);
        IntPoint pos(int(pTouchInput->x/100+0.5), int(pTouchInput->y/100+0.5));
        if (!(Player::get()->getDisplayEngine())->isFullscreen()) {
            pos -= IntPoint(winRect.left, winRect.top)+m_ClientAreaOffset;
            IntPoint winSize = IntPoint(Player::get()->getRootNode()->getSize());
            // Restrict coords to be inside window.
            pos.x = min(max(pos.x, 0), winSize.x-1);
            pos.y = min(max(pos.y, 0), winSize.y-1);
        }

        if (pTouchInput->dwFlags & TOUCHEVENTF_DOWN) {
//            cerr << "down: " << pos << endl; 
            m_LastID++;
            TouchEventPtr pEvent (new TouchEvent(m_LastID, Event::CURSOR_DOWN, pos,
                    Event::TOUCH));
            addTouchStatus((long)pTouchInput->dwID, pEvent);
        } else if (pTouchInput->dwFlags & TOUCHEVENTF_UP) {
//            cerr << "up: " << pos << endl; 
            TouchStatusPtr pTouchStatus = getTouchStatus(pTouchInput->dwID);
            TouchEventPtr pOldEvent = pTouchStatus->getLastEvent();
            TouchEventPtr pUpEvent(new TouchEvent(pOldEvent->getCursorID(), 
                    Event::CURSOR_UP, pos, Event::TOUCH));
            pTouchStatus->pushEvent(pUpEvent);
        } else if (pTouchInput->dwFlags & TOUCHEVENTF_MOVE) {
//            cerr << "motion: " << pos << endl; 
            TouchEventPtr pEvent(new TouchEvent(0, Event::CURSOR_MOTION, pos,
                    Event::TOUCH));
            TouchStatusPtr pTouchStatus = getTouchStatus((long)pTouchInput->dwID);
            AVG_ASSERT(pTouchStatus);
            pTouchStatus->pushEvent(pEvent);
        }
    }            
    delete [] pInputs;
    m_pCloseTouchInputHandleProc((HTOUCHINPUT)lParam);
#endif
}

IntPoint Win7TouchInputDevice::calcClientAreaOffset() const
{
    // TODO: This might work for now, but the x offset should probably be SM_CXBORDER.
    return IntPoint(GetSystemMetrics(SM_CYBORDER)+2, GetSystemMetrics(SM_CYCAPTION)+3);
}

}
