%ifdef _M_ARM64  
%include "config-arm64.asm"
%endif

%ifdef _M_X64  
%include "config-x64.asm"
%endif

%ifdef _M_IX86   
%include "config-x86.asm"
%endif