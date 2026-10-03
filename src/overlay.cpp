#include "overlay.h"
#include <windowsx.h>
#include <shlobj.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <string>

static const wchar_t* UI_CLS=L"NRLiveOverlayUI";
static const wchar_t* FPS_CLS=L"NRLiveMangoHud";
static HWND g_output=nullptr,g_ui=nullptr,g_settings=nullptr,g_fps=nullptr,g_pathEdit=nullptr;
static bool g_open=false,g_fsr=true,g_fpsVisible=true,g_initialized=false;
static bool g_toggleFsr=false,g_screenshot=false;
static OverlayHudConfig g_cfg{};
static float g_lastFps=0,g_lastMs=0; static Size g_cap{},g_out{};
static std::wstring g_shotPath;

static std::wstring iniPath(){ wchar_t p[MAX_PATH]{}; GetModuleFileNameW(nullptr,p,MAX_PATH); std::wstring s=p; auto n=s.find_last_of(L"\\/"); return s.substr(0,n+1)+L"scaleconfig.ini"; }
static void loadCfg(){
  wchar_t b[1024]{};
  g_fpsVisible=GetPrivateProfileIntW(L"FPS",L"enabled",1,iniPath().c_str())!=0;
  g_cfg.fps=GetPrivateProfileIntW(L"FPS",L"fps",1,iniPath().c_str())!=0;
  g_cfg.frametime=GetPrivateProfileIntW(L"FPS",L"frametime",1,iniPath().c_str())!=0;
  g_cfg.resolution=GetPrivateProfileIntW(L"FPS",L"resolution",1,iniPath().c_str())!=0;
  g_cfg.background=GetPrivateProfileIntW(L"FPS",L"background",1,iniPath().c_str())!=0;
  g_cfg.fontSize=std::clamp(GetPrivateProfileIntW(L"FPS",L"font_size",24,iniPath().c_str()),12,48);
  g_cfg.position=std::clamp(GetPrivateProfileIntW(L"FPS",L"position",0,iniPath().c_str()),0,3);
  wchar_t path[MAX_PATH*4]{}; GetPrivateProfileStringW(L"General",L"screenshot_path",L"",path,MAX_PATH*4,iniPath().c_str());
  if(path[0]) g_shotPath=path; else { SHGetFolderPathW(nullptr,CSIDL_MYPICTURES,nullptr,SHGFP_TYPE_CURRENT,path); g_shotPath=path; if(!g_shotPath.empty()&&g_shotPath.back()!=L'\\')g_shotPath+=L'\\'; g_shotPath+=L"NRLive"; }
}
static void saveCfg(){
  WritePrivateProfileStringW(L"FPS",L"enabled",g_fpsVisible?L"1":L"0",iniPath().c_str());
  WritePrivateProfileStringW(L"FPS",L"fps",g_cfg.fps?L"1":L"0",iniPath().c_str());
  WritePrivateProfileStringW(L"FPS",L"frametime",g_cfg.frametime?L"1":L"0",iniPath().c_str());
  WritePrivateProfileStringW(L"FPS",L"resolution",g_cfg.resolution?L"1":L"0",iniPath().c_str());
  WritePrivateProfileStringW(L"FPS",L"background",g_cfg.background?L"1":L"0",iniPath().c_str());
  wchar_t b[32]; swprintf_s(b,L"%d",g_cfg.fontSize); WritePrivateProfileStringW(L"FPS",L"font_size",b,iniPath().c_str());
  swprintf_s(b,L"%d",g_cfg.position); WritePrivateProfileStringW(L"FPS",L"position",b,iniPath().c_str());
  WritePrivateProfileStringW(L"General",L"screenshot_path",g_shotPath.c_str(),iniPath().c_str());
}
static void rr(HDC dc,RECT r,int radius,HBRUSH br){ RoundRect(dc,r.left,r.top,r.right,r.bottom,radius,radius); }
static void text(HDC dc,const wchar_t* s,RECT r,int size,COLORREF c,UINT flags=DT_CENTER|DT_VCENTER|DT_SINGLELINE){
  HFONT f=CreateFontW(-size,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");
  auto old=(HFONT)SelectObject(dc,f); SetBkMode(dc,TRANSPARENT); SetTextColor(dc,c); DrawTextW(dc,s,-1,&r,flags); SelectObject(dc,old); DeleteObject(f);
}
static void icon(HDC dc,RECT r,int kind,bool active){
  HPEN p=CreatePen(PS_SOLID,2,active?RGB(255,92,92):RGB(220,220,225)); auto op=(HPEN)SelectObject(dc,p); HBRUSH ob=(HBRUSH)SelectObject(dc,GetStockObject(NULL_BRUSH));
  int cx=(r.left+r.right)/2,cy=(r.top+r.bottom)/2;
  if(kind==0){ text(dc,L"FSR",RECT{cx-22,cy-10,cx+22,cy+10},12,active?RGB(255,92,92):RGB(220,220,225)); }
  else if(kind==1){ Ellipse(dc,cx-11,cy-11,cx+11,cy+11); text(dc,L"FPS",RECT{cx-20,cy-7,cx+20,cy+7},9,active?RGB(255,92,92):RGB(220,220,225)); }
  else if(kind==2){ RoundRect(dc,cx-13,cy-8,cx+13,cy+9,4,4); Ellipse(dc,cx-5,cy-4,cx+5,cy+6); MoveToEx(dc,cx-9,cy-8,nullptr); LineTo(dc,cx-5,cy-13); }
  else { Ellipse(dc,cx-4,cy-4,cx+4,cy+4); for(int i=0;i<8;i++){ double a=i*3.1415926535/4; MoveToEx(dc,cx+(int)(a?cos(a)*8:8),cy+(int)(sin(a)*8),nullptr); LineTo(dc,cx+(int)(cos(a)*13),cy+(int)(sin(a)*13)); } }
  SelectObject(dc,ob); SelectObject(dc,op); DeleteObject(p);
}
static void paintUI(HWND h,HDC dc){
  RECT rc{};GetClientRect(h,&rc); HBRUSH bg=CreateSolidBrush(RGB(24,25,31));FillRect(dc,&rc,bg);DeleteObject(bg);
  const int s=56,g=10,left=16,top=12;
  for(int i=0;i<4;i++){RECT b{left+i*(s+g),top,left+i*(s+g)+s,top+s};HBRUSH br=CreateSolidBrush(i==3&&g_settings?RGB(55,28,31):RGB(38,40,48));rr(dc,b,10,br);DeleteObject(br);icon(dc,b,i,(i==0&&g_fsr)||(i==1&&g_fpsVisible)||(i==3&&g_settings));}
}
static void paintSettings(HWND h,HDC dc){
  RECT rc{};GetClientRect(h,&rc);HBRUSH bg=CreateSolidBrush(RGB(28,29,36));FillRect(dc,&rc,bg);DeleteObject(bg);
  text(dc,L"NRLive Settings",RECT{24,18,rc.right-24,54},24,RGB(245,245,248),DT_LEFT|DT_VCENTER|DT_SINGLELINE);
  text(dc,L"FPS overlay",RECT{24,72,240,104},16,RGB(225,225,230),DT_LEFT|DT_VCENTER|DT_SINGLELINE);
  text(dc,L"Display",RECT{24,116,240,144},14,RGB(170,170,180),DT_LEFT|DT_VCENTER|DT_SINGLELINE);
  text(dc,L"Screenshot folder",RECT{24,204,240,232},14,RGB(170,170,180),DT_LEFT|DT_VCENTER|DT_SINGLELINE);
  text(dc,L"MangoHud-style metrics and positioning are stored in scaleconfig.ini.",RECT{24,280,rc.right-24,320},12,RGB(135,135,145),DT_LEFT|DT_TOP|DT_WORDBREAK);
}
static LRESULT CALLBACK settingsProc(HWND h,UINT m,WPARAM w,LPARAM l){
  if(m==WM_PAINT){PAINTSTRUCT ps;HDC dc=BeginPaint(h,&ps);paintSettings(h,dc);EndPaint(h,&ps);return 0;}
  if(m==WM_COMMAND){
    if(LOWORD(w)==101){g_cfg.fps=IsDlgButtonChecked(h,101)==BST_CHECKED;saveCfg();InvalidateRect(g_fps,nullptr,FALSE);}
    if(LOWORD(w)==102){g_cfg.frametime=IsDlgButtonChecked(h,102)==BST_CHECKED;saveCfg();}
    if(LOWORD(w)==103){g_cfg.resolution=IsDlgButtonChecked(h,103)==BST_CHECKED;saveCfg();}
    if(HIWORD(w)==EN_CHANGE && LOWORD(w)==104){ wchar_t p[MAX_PATH*4]{}; GetWindowTextW(g_pathEdit,p,MAX_PATH*4); g_shotPath=p; saveCfg(); return 0; }
    if(LOWORD(w)==105){
      wchar_t p[MAX_PATH*4]{};BROWSEINFOW bi{};bi.hwndOwner=h;bi.lpszTitle=L"Choose screenshot folder";LPITEMIDLIST id=SHBrowseForFolderW(&bi);
      if(id){SHGetPathFromIDListW(id,p);CoTaskMemFree(id);if(p[0]){g_shotPath=p;saveCfg();}}
    }
  }
  return DefWindowProcW(h,m,w,l);
}
static LRESULT CALLBACK uiProc(HWND h,UINT m,WPARAM w,LPARAM l){
  if(m==WM_PAINT){PAINTSTRUCT ps;HDC dc=BeginPaint(h,&ps);paintUI(h,dc);EndPaint(h,&ps);return 0;}
  if(m==WM_LBUTTONUP){POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)};const int s=56,g=10,left=16;
    for(int i=0;i<4;i++){RECT b{left+i*(s+g),12,left+i*(s+g)+s,68};if(PtInRect(&b,p)){
      if(i==0){g_fsr=!g_fsr;g_toggleFsr=true;}
      else if(i==1){g_fpsVisible=!g_fpsVisible;saveCfg();updateFpsPos();}
      else if(i==2){g_screenshot=true;}
      else { if(g_settings){ if(IsWindowVisible(g_settings)){ ShowWindow(g_settings,SW_HIDE); } else { if(g_pathEdit) SetWindowTextW(g_pathEdit,g_shotPath.c_str()); ShowWindow(g_settings,SW_SHOWNOACTIVATE); InvalidateRect(g_settings,nullptr,FALSE); } } }
      InvalidateRect(h,nullptr,FALSE); return 0;}}
  } return DefWindowProcW(h,m,w,l);
}
static void paintFps(HDC dc){
  RECT rc{};GetClientRect(g_fps,&rc);int pad=12;HBRUSH bg=CreateSolidBrush(g_cfg.background?RGB(12,13,16):RGB(0,0,0));FillRect(dc,&rc,bg);DeleteObject(bg);
  int y=pad; if(g_cfg.fps){wchar_t b[64];swprintf_s(b,L"%.1f FPS",g_lastFps);text(dc,b,RECT{pad,y,rc.right-pad,y+g_cfg.fontSize+4},g_cfg.fontSize,RGB(255,255,255),DT_LEFT|DT_VCENTER|DT_SINGLELINE);y+=g_cfg.fontSize+8;}
  if(g_cfg.frametime){wchar_t b[64];swprintf_s(b,L"%.2f ms",g_lastMs);text(dc,b,RECT{pad,y,rc.right-pad,y+20},16,RGB(190,190,198),DT_LEFT|DT_VCENTER|DT_SINGLELINE);y+=24;}
  if(g_cfg.resolution){wchar_t b[64];swprintf_s(b,L"%ux%u",g_cap.w,g_cap.h);text(dc,b,RECT{pad,y,rc.right-pad,y+20},16,RGB(150,150,160),DT_LEFT|DT_VCENTER|DT_SINGLELINE);}
}
static void updateFpsPos(){
  if(!g_fps||!IsWindow(g_fps)||!g_output)return;RECT o{};GetWindowRect(g_output,&o);int w=190,h=(g_cfg.fps?g_cfg.fontSize+16:0)+(g_cfg.frametime?24:0)+(g_cfg.resolution?24:0)+24;int x=o.left+16,y=o.top+16;
  if(g_cfg.position==1)x=o.right-w-16; if(g_cfg.position==2)y=o.bottom-h-16; if(g_cfg.position==3){x=o.right-w-16;y=o.bottom-h-16;}
  SetWindowPos(g_fps,HWND_TOPMOST,x,y,w,h,SWP_NOACTIVATE|(g_fpsVisible?SWP_SHOWWINDOW:SWP_HIDEWINDOW));
}
static LRESULT CALLBACK fpsProc(HWND h,UINT m,WPARAM w,LPARAM l){if(m==WM_PAINT){PAINTSTRUCT ps;HDC dc=BeginPaint(h,&ps);paintFps(dc);EndPaint(h,&ps);return 0;}if(m==WM_NCHITTEST)return HTTRANSPARENT;return DefWindowProcW(h,m,w,l);}
bool overlayInit(HINSTANCE inst,HWND output){
  if(g_initialized)return true;g_output=output;loadCfg();
  WNDCLASSEXW c{sizeof(c)};c.hInstance=inst;c.lpfnWndProc=uiProc;c.lpszClassName=UI_CLS;c.hCursor=LoadCursor(nullptr,IDC_ARROW);c.hbrBackground=nullptr;RegisterClassExW(&c);
  WNDCLASSEXW f{sizeof(f)};f.hInstance=inst;f.lpfnWndProc=fpsProc;f.lpszClassName=FPS_CLS;f.hCursor=nullptr;f.hbrBackground=nullptr;RegisterClassExW(&f);
  g_ui=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,UI_CLS,L"",WS_CHILD,0,0,292,80,output,nullptr,inst,nullptr);
  g_fps=CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TOPMOST,FPS_CLS,L"",WS_POPUP,0,0,190,100,nullptr,nullptr,inst,nullptr);
  if(g_fps) SetLayeredWindowAttributes(g_fps,0,255,LWA_ALPHA);
  WNDCLASSEXW sc{sizeof(sc)};sc.hInstance=inst;sc.lpfnWndProc=settingsProc;sc.lpszClassName=L"NRLiveSettings";sc.hCursor=LoadCursor(nullptr,IDC_ARROW);sc.hbrBackground=nullptr;RegisterClassExW(&sc);
  g_settings=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"NRLiveSettings",L"",WS_CHILD,320,90,520,350,output,nullptr,inst,nullptr);
  if(g_settings){
    CreateWindowW(L"BUTTON",L"FPS",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,24,92,100,26,g_settings,(HMENU)101,inst,nullptr);
    CreateWindowW(L"BUTTON",L"Frametime",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,130,92,120,26,g_settings,(HMENU)102,inst,nullptr);
    CreateWindowW(L"BUTTON",L"Resolution",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,260,92,120,26,g_settings,(HMENU)103,inst,nullptr);
    g_pathEdit=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",g_shotPath.c_str(),WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,24,150,380,28,g_settings,(HMENU)104,inst,nullptr);
    CreateWindowW(L"BUTTON",L"Browse...",WS_CHILD|WS_VISIBLE,414,150,82,28,g_settings,(HMENU)105,inst,nullptr);
    CheckDlgButton(g_settings,101,g_cfg.fps?BST_CHECKED:BST_UNCHECKED);
    CheckDlgButton(g_settings,102,g_cfg.frametime?BST_CHECKED:BST_UNCHECKED);
    CheckDlgButton(g_settings,103,g_cfg.resolution?BST_CHECKED:BST_UNCHECKED);
    ShowWindow(g_settings,SW_HIDE);
  }
  g_initialized=true;updateFpsPos();return true;
}
void overlayShutdown(){saveCfg();if(g_fps)DestroyWindow(g_fps);if(g_ui)DestroyWindow(g_ui);g_fps=g_ui=nullptr;g_initialized=false;}
void overlaySetOpen(bool open){g_open=open;if(g_ui)ShowWindow(g_ui,open?SW_SHOWNOACTIVATE:SW_HIDE);if(!open&&g_settings)ShowWindow(g_settings,SW_HIDE);updateFpsPos();}
void overlayUpdate(float fps,float ms,Size cap,Size out){g_lastFps=fps;g_lastMs=ms;g_cap=cap;g_out=out;if(g_fps){if(g_fpsVisible)InvalidateRect(g_fps,nullptr,FALSE);updateFpsPos();}}
void overlaySetFsrEnabled(bool e){g_fsr=e;if(g_ui)InvalidateRect(g_ui,nullptr,FALSE);}
bool overlayConsumeFsrToggle(){bool v=g_toggleFsr;g_toggleFsr=false;return v;}
bool overlayConsumeScreenshot(){bool v=g_screenshot;g_screenshot=false;return v;}
const OverlayHudConfig& overlayConfig(){return g_cfg;}

std::wstring overlayScreenshotPath(){ return g_shotPath; }
