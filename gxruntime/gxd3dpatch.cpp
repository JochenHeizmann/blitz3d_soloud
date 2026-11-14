// In-memory patch for the 2048x2048 render target limit of Direct3D 7.
//
// IDirect3D7::CreateDevice (and a second D3D7 entry point) in d3dim700.dll reject render
// targets wider or higher than 2048 pixels, although every halfway modern driver handles
// much larger surfaces. The check looks like this (d3dim700.dll 10.0.19041.1, both sites):
//
//   B8 00 08 00 00     mov eax,0x800
//   39 45 xx           cmp [ebp+disp],eax      (width;  disp8 or disp32 form 39 85 xxxxxxxx)
//   0F 87 xxxxxxxx     ja  fail
//   39 45 xx           cmp [ebp+disp],eax      (height)
//   0F 87 xxxxxxxx     ja  fail                (same target)
//   ...
//   fail: B8 82 00 76 88   mov eax,DDERR_INVALIDOBJECT   or
//         BE 57 00 07 80   mov esi,E_INVALIDARG
//
// The 0x800 is replaced by 0xFFFFFFFF, so the unsigned 'ja' never jumps. Only applied if the
// .text section contains exactly the two expected sites and every byte matches; otherwise
// nothing is changed. Same idea as LegacyD3DResolutionHack, but in memory for this process only.
//
// Called by the runtime before DirectDraw is first used: the system d3dim700.dll is loaded
// here and never released, so ddraw later gets this (patched) instance.

#include "std.h"
#include "gxd3dpatch.h"

// cmp [ebp+disp8],eax (39 45 xx) or cmp [ebp+disp32],eax (39 85 xx xx xx xx)
static int cmpEaxLen( const BYTE *p ){
	if( p[0]!=0x39 ) return 0;
	if( p[1]==0x45 ) return 3;
	if( p[1]==0x85 ) return 6;
	return 0;
}

static bool matchSite( const BYTE *p,const BYTE *beg,const BYTE *end ){
	if( p+5+2*(6+6)>end || p[0]!=0xB8 || *(const DWORD*)(p+1)!=0x800 ) return false;
	const BYTE *q=p+5,*target=0;
	for( int i=0;i<2;++i ){
		int n=cmpEaxLen( q );
		if( !n ) return false;
		q+=n;
		if( q[0]!=0x0F || q[1]!=0x87 ) return false;		// ja rel32
		const BYTE *t=q+6+*(const int*)(q+2);
		q+=6;
		if( !i ) target=t;
		else if( t!=target ) return false;
	}
	if( target<beg || target+5>end ) return false;
	static const BYTE errInvalidObject[]={ 0xB8,0x82,0x00,0x76,0x88 };	// mov eax,0x88760082
	static const BYTE errInvalidArg[]={ 0xBE,0x57,0x00,0x07,0x80 };		// mov esi,0x80070057
	return !memcmp( target,errInvalidObject,5 ) || !memcmp( target,errInvalidArg,5 );
}

bool gxD3DUnlockResolution(){
	char path[MAX_PATH];
	if( !GetSystemDirectoryA( path,MAX_PATH-32 ) ) return false;
	strcat( path,"\\d3dim700.dll" );

	// already loaded by someone else: must be the system dll (not a proxy/wrapper)
	if( HMODULE loaded=GetModuleHandleA( "d3dim700.dll" ) ){
		char p[MAX_PATH];
		if( !GetModuleFileNameA( loaded,p,MAX_PATH ) || _stricmp( p,path ) ) return false;
	}
	HMODULE mod=LoadLibraryA( path );		// never released, see above
	if( !mod ) return false;

	BYTE *base=(BYTE*)mod;
	IMAGE_NT_HEADERS *nt=(IMAGE_NT_HEADERS*)(base+((IMAGE_DOS_HEADER*)base)->e_lfanew);
	IMAGE_SECTION_HEADER *sec=IMAGE_FIRST_SECTION( nt );
	BYTE *beg=0,*end=0;
	for( int k=0;k<nt->FileHeader.NumberOfSections;++k,++sec ){
		if( !memcmp( sec->Name,".text\0\0\0",8 ) ){
			beg=base+sec->VirtualAddress;
			end=beg+sec->Misc.VirtualSize;
			break;
		}
	}
	if( !beg ) return false;

	// every 'mov eax,0x800 / cmp' candidate must be a full match, and there must be exactly two
	BYTE *sites[2];
	int n=0;
	for( BYTE *p=beg;p+6<=end;++p ){
		if( p[0]!=0xB8 || p[1]!=0x00 || p[2]!=0x08 || p[3]!=0x00 || p[4]!=0x00 || p[5]!=0x39 ) continue;
		if( n==2 || !matchSite( p,beg,end ) ) return false;
		sites[n++]=p+1;
	}
	if( n!=2 ) return false;

	for( int k=0;k<2;++k ){
		DWORD old;
		if( !VirtualProtect( sites[k],4,PAGE_EXECUTE_READWRITE,&old ) ) return false;
		*(DWORD*)sites[k]=0xFFFFFFFF;
		VirtualProtect( sites[k],4,old,&old );
	}
	FlushInstructionCache( GetCurrentProcess(),0,0 );
	return true;
}
