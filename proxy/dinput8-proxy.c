/* dinput8.dll proxy for Grim Dawn x64: loads the GrimDawnNativeMap modules without the tray exe.
   Install: copy dinput8.dll (+ optional GrimDawnNativeMap.ini) into "Grim Dawn\x64". Remove the file to uninstall.
   - All DirectInput calls are forwarded to the real System32\dinput8.dll.
   - DllMain only starts a thread (no work under the loader lock).
   - The thread waits for Game.dll and Engine.dll, checks the supported game build (same SHA-256 values as the
     tray loaders) and then loads the four modules in the tray app's order. Each module initializes itself. */
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

static HMODULE real_dinput;
static wchar_t folder[MAX_PATH], log_file[MAX_PATH];

static void plog(const char *msg){
    FILE *f;if(!log_file[0])return;f=_wfopen(log_file,L"a");
    if(f){SYSTEMTIME t;GetLocalTime(&t);fprintf(f,"%04d-%02d-%02d %02d:%02d:%02d %s\n",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,msg);fclose(f);}
}
static FARPROC real(const char *name){
    if(!real_dinput){wchar_t path[MAX_PATH];UINT n=GetSystemDirectoryW(path,MAX_PATH);
        if(n && n<MAX_PATH-16){wcscat(path,L"\\dinput8.dll");real_dinput=LoadLibraryW(path);}}
    return real_dinput?GetProcAddress(real_dinput,name):NULL;
}
__declspec(dllexport) HRESULT WINAPI DirectInput8Create(HINSTANCE a,DWORD b,const GUID *c,LPVOID *d,void *e){
    typedef HRESULT (WINAPI *F)(HINSTANCE,DWORD,const GUID*,LPVOID*,void*);F f=(F)real("DirectInput8Create");
    return f?f(a,b,c,d,e):E_FAIL;
}
__declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void){typedef HRESULT (WINAPI *F)(void);F f=(F)real("DllCanUnloadNow");return f?f():S_FALSE;}
__declspec(dllexport) HRESULT WINAPI DllGetClassObject(const GUID *a,const GUID *b,LPVOID *c){
    typedef HRESULT (WINAPI *F)(const GUID*,const GUID*,LPVOID*);F f=(F)real("DllGetClassObject");return f?f(a,b,c):E_FAIL;
}
__declspec(dllexport) HRESULT WINAPI DllRegisterServer(void){typedef HRESULT (WINAPI *F)(void);F f=(F)real("DllRegisterServer");return f?f():E_FAIL;}
__declspec(dllexport) HRESULT WINAPI DllUnregisterServer(void){typedef HRESULT (WINAPI *F)(void);F f=(F)real("DllUnregisterServer");return f?f():E_FAIL;}
__declspec(dllexport) LPCVOID WINAPI GetdfDIJoystick(void){typedef LPCVOID (WINAPI *F)(void);F f=(F)real("GetdfDIJoystick");return f?f():NULL;}

/* SHA-256 through bcrypt.dll (loaded at run time). Returns 1 when the file matches. */
static int file_hash_is(const wchar_t *path,const char *expected){
    typedef LONG (WINAPI *Open)(void**,LPCWSTR,LPCWSTR,ULONG);
    typedef LONG (WINAPI *Hash)(void*,PUCHAR,ULONG,PUCHAR,ULONG,PUCHAR,ULONG);
    typedef LONG (WINAPI *Close)(void*,ULONG);
    HMODULE b=LoadLibraryW(L"bcrypt.dll");Open open;Hash hash;Close close;void *alg=NULL;
    HANDLE h;DWORD size,got;unsigned char *data,digest[32];char hex[65];int i,ok=0;
    if(!b)return 0;
    open=(Open)GetProcAddress(b,"BCryptOpenAlgorithmProvider");hash=(Hash)GetProcAddress(b,"BCryptHash");close=(Close)GetProcAddress(b,"BCryptCloseAlgorithmProvider");
    if(!open || !hash || !close || open(&alg,L"SHA256",NULL,0)!=0){
#ifdef PROXY_TEST
        printf("bcrypt open failed %p %p %p\n",open,hash,close);
#endif
        return 0;}
    h=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
#ifdef PROXY_TEST
    printf("open h=%p err=%lu\n",h,GetLastError());
#endif
    if(h!=INVALID_HANDLE_VALUE){
        size=GetFileSize(h,NULL);data=(unsigned char*)HeapAlloc(GetProcessHeap(),0,size?size:1);
#ifdef PROXY_TEST
        {BOOL r=ReadFile(h,data,size,&got,NULL);LONG hr=hash(alg,NULL,0,data,size,digest,32);printf("size=%lu read=%d got=%lu hash=%lx\n",size,r,got,hr);SetFilePointer(h,0,NULL,FILE_BEGIN);}
#endif
        if(data && size!=INVALID_FILE_SIZE && ReadFile(h,data,size,&got,NULL) && got==size && hash(alg,NULL,0,data,size,digest,32)==0){
            for(i=0;i<32;i++)sprintf(hex+i*2,"%02X",digest[i]);
            ok=_stricmp(hex,expected)==0;
#ifdef PROXY_TEST
            printf("hex=%s\n",hex);
#endif
        }
        if(data)HeapFree(GetProcessHeap(),0,data);CloseHandle(h);
    }
    close(alg,0);return ok;
}
static int supported_build(void){
    wchar_t dir[MAX_PATH],path[MAX_PATH],*slash;
    GetModuleFileNameW(NULL,dir,MAX_PATH);slash=wcsrchr(dir,L'\\');if(!slash)return 0;*slash=0;
    _snwprintf(path,MAX_PATH-1,L"%ls\\Grim Dawn.exe",dir);if(!file_hash_is(path,"D3915E2599317B6670CC088BA958AFB2B2F4FA8141CFD1D777EE02F9EA1FC4C4"))return 0;
    _snwprintf(path,MAX_PATH-1,L"%ls\\Game.dll",dir);if(!file_hash_is(path,"CD7ECD7942D719F96B42AFF4896F8C25D52152B4A41191F71A4A66EA48CECAD2"))return 0;
    _snwprintf(path,MAX_PATH-1,L"%ls\\Engine.dll",dir);if(!file_hash_is(path,"4C2EA14CE1C591B315CC29A8A96819A5C3C7E194ADBD6F7EEA208B9BB8A93153"))return 0;
    return 1;
}
static DWORD WINAPI worker(void *self){
    /* map pack modules live in `folder` (ini or x64\GrimDawnNativeMap); the Korean voice pack's note player
       lives in x64\GrimDawnKoreanVoice. Either pack may be installed alone; missing modules are skipped. */
    static const wchar_t *modules[]={L"native-atlas-v1.4.dll",L"native-radar-v1.1.dll",L"uniquetab-ko-v1.1.1.asi",L"native-korean-v1.3.1.dll",L"native-lorevo.dll"};
    static const char *status[]={"AtlasStatus","RadarStatus","CollectionStatus","ImeStatus","LoreStatus"};
    static wchar_t voice[MAX_PATH];
    wchar_t here[MAX_PATH],ini[MAX_PATH],path[MAX_PATH],*slash;char msg[600];int i,waited;
    GetModuleFileNameW((HMODULE)self,here,MAX_PATH);slash=wcsrchr(here,L'\\');if(slash)*slash=0;
    _snwprintf(ini,MAX_PATH-1,L"%ls\\GrimDawnNativeMap.ini",here);
    GetPrivateProfileStringW(L"GrimDawnNativeMap",L"folder",L"",folder,MAX_PATH,ini);
    if(!folder[0])_snwprintf(folder,MAX_PATH-1,L"%ls\\GrimDawnNativeMap",here);
    _snwprintf(voice,MAX_PATH-1,L"%ls\\GrimDawnKoreanVoice",here);
    _snwprintf(log_file,MAX_PATH-1,L"%ls\\dinput8-proxy.log",
               GetFileAttributesW(folder)!=INVALID_FILE_ATTRIBUTES?folder:voice);
    sprintf(msg,"START folder=%ls",folder);plog(msg);
    for(waited=0;!(GetModuleHandleW(L"Game.dll") && GetModuleHandleW(L"Engine.dll"));waited++){
        if(waited>1200){plog("STOP Game.dll/Engine.dll never loaded");return 0;}
        Sleep(100);
    }
    if(!supported_build()){plog("STOP unsupported game build (exe/Game.dll/Engine.dll hash); nothing loaded");return 0;}
    Sleep(1500);   /* the tray app injected 0-2 s after both modules appeared */
    for(i=0;i<5;i++){
        HMODULE m;DWORD st=0;int t;
        _snwprintf(path,MAX_PATH-1,L"%ls\\%ls",i<4?folder:voice,modules[i]);
        if(GetFileAttributesW(path)==INVALID_FILE_ATTRIBUTES){sprintf(msg,"SKIP missing %ls",path);plog(msg);continue;}
        m=LoadLibraryW(path);
        if(!m){sprintf(msg,"FAIL LoadLibrary %ls error=%lu",modules[i],GetLastError());plog(msg);continue;}
        {   typedef DWORD (WINAPI *S)(void*);S s=(S)GetProcAddress(m,status[i]);
            for(t=0;s && t<600;t++){st=s(NULL);if(st!=0 && st!=1)break;Sleep(50);}
            sprintf(msg,"LOADED %ls status=%lu",modules[i],st);plog(msg);
        }
    }
    plog("DONE");return 0;
}
BOOL WINAPI DllMain(HINSTANCE dll,DWORD why,LPVOID reserved){
    if(why==DLL_PROCESS_ATTACH){
        HANDLE t;DisableThreadLibraryCalls(dll);
        t=CreateThread(NULL,0,worker,dll,0,NULL);if(t)CloseHandle(t);
    }
    return TRUE;
}
