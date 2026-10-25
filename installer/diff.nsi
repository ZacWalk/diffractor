;NSIS Modern User Interface

!define PRODUCT_NAME "Diffractor"
!define PRODUCT32_EXE "diffractor32.exe"
!define PRODUCT64_EXE "diffractor64.exe"
!define PRODUCT_PUBLISHER "Diffractor"
!define BUILD_NUM "1313"
!define PRODUCT_VERSION "127.3"
!define FILE_VERSION "1.27.3.${BUILD_NUM}"
!define PRODUCT_WEB_SITE "http://www.Diffractor.com/"
!define PRODUCT_STARTMENU_REGVAL "NSIS:StartMenuDir"
!define PRODUCT_UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}"
!define PRODUCT_SETTINGS_KEY "Software\${PRODUCT_NAME}"
!define PRODUCT_DEFAULT_DIR_KEY "${PRODUCT_SETTINGS_KEY}\InstallDir"
!define PRODUCT_UNINST_ROOT_KEY "HKLM"

; Language Selection Dialog Settings
!define MUI_LANGDLL_REGISTRY_ROOT "${PRODUCT_UNINST_ROOT_KEY}"
!define MUI_LANGDLL_REGISTRY_KEY "${PRODUCT_UNINST_KEY}"
!define MUI_LANGDLL_REGISTRY_VALUENAME "NSIS:Language"
!define MUI_PAGE_CUSTOMFUNCTION_SHOW MyWelcomeShowCallback

Unicode True

;--------------------------------
;Include Modern UI

!include nsDialogs.nsh
!include MUI2.nsh
!include WinMessages.nsh 
!include WinVer.nsh
!include FileFunc.nsh
!include StrFunc.nsh
!include x64.nsh

${StrStr} # Supportable for Install Sections and Functions
 
Function un.CloseProgram 
  Exch $1
  Push $0
  loop:
    FindWindow $0 $1
    IntCmp $0 0 done
      SendMessage $0 ${WM_CLOSE} 0 0
    Sleep 100 
    Goto loop 
  done: 
  Pop $0 
  Pop $1
FunctionEnd

Function .CloseProgram 
  Exch $1
  Push $0
  loop:
    FindWindow $0 $1
    IntCmp $0 0 done
      SendMessage $0 ${WM_CLOSE} 0 0
    Sleep 100 
    Goto loop 
  done: 
  Pop $0 
  Pop $1
FunctionEnd


;--------------------------------

!ifdef SHCNE_ASSOCCHANGED
!undef SHCNE_ASSOCCHANGED
!endif
!define SHCNE_ASSOCCHANGED 0x08000000

!ifdef SHCNF_FLUSH
!undef SHCNF_FLUSH
!endif
!define SHCNF_FLUSH        0x1000

!ifdef SHCNF_IDLIST
!undef SHCNF_IDLIST
!endif
!define SHCNF_IDLIST       0x0000

!macro UPDATEFILEASSOC
  IntOp $1 ${SHCNE_ASSOCCHANGED} | 0
  IntOp $0 ${SHCNF_IDLIST} | ${SHCNF_FLUSH}
; Using the system.dll plugin to call the SHChangeNotify Win32 API function so we
; can update the shell.
  System::Call "shell32::SHChangeNotify(i,i,i,i) (${SHCNE_ASSOCCHANGED}, $0, 0, 0)"
!macroend

;--------------------------------
;General

Name "${PRODUCT_NAME}"
Caption "${PRODUCT_NAME} ${PRODUCT_VERSION} Setup"
BrandingText "${PRODUCT_NAME} ${PRODUCT_VERSION}.${BUILD_NUM}"

OutFile "..\diffractor-setup.exe"
InstallDir "$LOCALAPPDATA\${PRODUCT_NAME}"
InstallDirRegKey HKLM "${PRODUCT_DEFAULT_DIR_KEY}" ""
ShowInstDetails show
ShowUnInstDetails show
SetCompressor /SOLID /FINAL lzma
SetCompressorDictSize 128
Icon "Diffractor.ico"
XPStyle on
RequestExecutionLevel user

VIProductVersion "${FILE_VERSION}"
VIAddVersionKey ProductName "${PRODUCT_NAME}"
VIAddVersionKey ProductVersion "${FILE_VERSION}"
VIAddVersionKey CompanyName "${PRODUCT_PUBLISHER}"
VIAddVersionKey CompanyWebsite "${PRODUCT_WEB_SITE}"
VIAddVersionKey FileVersion "${FILE_VERSION}"
VIAddVersionKey FileDescription ""
VIAddVersionKey LegalCopyright "Copyright (C) 2026 Zac Walker"



;--------------------------------
;Interface Settings

;Show all languages, despite user's codepage
!define MUI_LANGDLL_ALLLANGUAGES

!define MUI_ABORTWARNING
!define MUI_ICON "Diffractor.ico"
!define MUI_UNICON "Diffractor.ico"
!define MUI_HEADERIMAGE
!define MUI_HEADERIMAGE_BITMAP "logo.bmp"
!define MUI_HEADERIMAGE_BITMAP_NOSTRETCH
!define MUI_HEADERIMAGE_RIGHT
!define MUI_WELCOMEFINISHPAGE_BITMAP "install.bmp"
!define MUI_UNWELCOMEFINISHPAGE_BITMAP "uninstall.bmp"
!define MUI_COMPONENTSPAGE_SMALLDESC



;--------------------------------
;Pages

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "License.txt"
;!insertmacro MUI_PAGE_COMPONENTS
;!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_FUNCTION "StartDiffractor"
!insertmacro MUI_PAGE_FINISH
  
!insertmacro MUI_UNPAGE_WELCOME
;!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH 

;--------------------------------
;Languages

!insertmacro MUI_LANGUAGE "English" ; The first language is the default language
!insertmacro MUI_LANGUAGE "Czech"
!insertmacro MUI_LANGUAGE "German"
!insertmacro MUI_LANGUAGE "Spanish"
!insertmacro MUI_LANGUAGE "French"
!insertmacro MUI_LANGUAGE "Italian"
!insertmacro MUI_LANGUAGE "Japanese"
!insertmacro MUI_LANGUAGE "Korean"
!insertmacro MUI_LANGUAGE "Polish"
!insertmacro MUI_LANGUAGE "PortugueseBR"
!insertmacro MUI_LANGUAGE "Russian"
!insertmacro MUI_LANGUAGE "Turkish"
!insertmacro MUI_LANGUAGE "Ukrainian"
!insertmacro MUI_LANGUAGE "SimpChinese"

LangString WELCOME_TEXT ${LANG_ENGLISH} "Superfast searching, viewing and comparing of photos or videos.$\r$\n$\r$\nOptimized for your graphics card and PC."
LangString WELCOME_TEXT ${LANG_CZECH} "Lehká správa fotografií a médií.$\r$\n$\r$\nOptimalizováno pro vaši grafickou kartu a PC."
LangString WELCOME_TEXT ${LANG_GERMAN} "Superschnelle Suche, Betrachtung und Vergleich von Fotos oder Videos.$\r$\n$\r$\nOptimiert für Ihre Grafikkarte und Ihren PC."
LangString WELCOME_TEXT ${LANG_SPANISH} "Búsqueda, visualización y comparación ultrarrápidas de fotos o vídeos.$\r$\n$\r$\nOptimizado para su tarjeta gráfica y su PC."
LangString WELCOME_TEXT ${LANG_FRENCH} "Recherche, affichage et comparaison ultrarapides de photos ou de vidéos.$\r$\n$\r$\nOptimisé pour votre carte graphique et votre PC."
LangString WELCOME_TEXT ${LANG_ITALIAN} "Ricerca, visualizzazione e confronto velocissimi di foto o video.$\r$\n$\r$\nOttimizzato per la scheda grafica e il PC."
LangString WELCOME_TEXT ${LANG_JAPANESE} "写真やビデオを超高速で検索、表示、比較します。$\r$\n$\r$\nグラフィックス カードと PC に合わせて最適化されています。"
LangString WELCOME_TEXT ${LANG_KOREAN} "사진과 비디오를 매우 빠르게 검색하고 보고 비교하세요.$\r$\n$\r$\n그래픽 카드와 PC에 최적화되어 있습니다."
LangString WELCOME_TEXT ${LANG_POLISH} "Błyskawiczne wyszukiwanie, wyświetlanie i porównywanie zdjęć oraz filmów.$\r$\n$\r$\nZoptymalizowano pod kątem karty graficznej i komputera."
LangString WELCOME_TEXT ${LANG_PORTUGUESEBR} "Pesquisa, visualização e comparação ultrarrápidas de fotos ou vídeos.$\r$\n$\r$\nOtimizado para sua placa de vídeo e seu PC."
LangString WELCOME_TEXT ${LANG_RUSSIAN} "Сверхбыстрый поиск, просмотр и сравнение фотографий и видео.$\r$\n$\r$\nОптимизировано для вашей видеокарты и компьютера."
LangString WELCOME_TEXT ${LANG_TURKISH} "Fotoğraf ve videolarda son derece hızlı arama, görüntüleme ve karşılaştırma.$\r$\n$\r$\nEkran kartınız ve bilgisayarınız için optimize edilmiştir."
LangString WELCOME_TEXT ${LANG_UKRAINIAN} "Надшвидкий пошук, перегляд і порівняння фотографій та відео.$\r$\n$\r$\nОптимізовано для вашої відеокарти та комп’ютера."
LangString WELCOME_TEXT ${LANG_SIMPCHINESE} "超快速搜索、查看和比较照片或视频。$\r$\n$\r$\n针对您的显卡和电脑进行了优化。"

LangString APP_LANG ${LANG_ENGLISH} "en"
LangString APP_LANG ${LANG_CZECH} "cs"
LangString APP_LANG ${LANG_GERMAN} "de"
LangString APP_LANG ${LANG_SPANISH} "es"
LangString APP_LANG ${LANG_FRENCH} "fr"
LangString APP_LANG ${LANG_ITALIAN} "it"
LangString APP_LANG ${LANG_JAPANESE} "ja"
LangString APP_LANG ${LANG_KOREAN} "ko"
LangString APP_LANG ${LANG_POLISH} "pl"
LangString APP_LANG ${LANG_PORTUGUESEBR} "pt"
LangString APP_LANG ${LANG_RUSSIAN} "ru"
LangString APP_LANG ${LANG_TURKISH} "tr"
LangString APP_LANG ${LANG_UKRAINIAN} "uk"
LangString APP_LANG ${LANG_SIMPCHINESE} "zh"

;--------------------------------
;Reserve Files
  
;If you are using solid compression, files that are required before
;the actual installation should be stored first in the data block,
;because this will make your installer start faster.
  
!insertmacro MUI_RESERVEFILE_LANGDLL

Function .onInit
  ${IfNot} ${AtLeastWin7}
    MessageBox MB_OK "Sadly, Diffractor requires Windows 7 or above."
    Quit
  ${EndIf}

  ; Detect an existing installation so we can treat this run as an update.
  ; On an update we must NOT recreate the desktop shortcut: if the user has
  ; deleted it, recreating it on every update is unwanted (issue #173). The
  ; uninstall key is written on every install, so its presence means Diffractor
  ; is already installed.
  Var /GLOBAL IS_UPDATE
  StrCpy $IS_UPDATE "0"
  Var /GLOBAL EXISTING_INSTALL
  ReadRegStr $EXISTING_INSTALL SHCTX "${PRODUCT_UNINST_KEY}" "DisplayName"
  ${If} $EXISTING_INSTALL != ""
    StrCpy $IS_UPDATE "1"
  ${EndIf}

  Var /GLOBAL DEF_LANG
  ReadRegStr $DEF_LANG HKCU "${PRODUCT_SETTINGS_KEY}" "lang"

  ${If} $DEF_LANG == "de"
	StrCpy $LANGUAGE 1031
  ${ElseIf} $DEF_LANG == "cs"
	StrCpy $LANGUAGE 1029
  ${ElseIf} $DEF_LANG == "es"
	StrCpy $LANGUAGE 1034
	${ElseIf} $DEF_LANG == "fr"
	StrCpy $LANGUAGE 1036
	${ElseIf} $DEF_LANG == "it"
	StrCpy $LANGUAGE 1040
  ${ElseIf} $DEF_LANG == "ja"
	StrCpy $LANGUAGE 1041
	${ElseIf} $DEF_LANG == "ko"
	StrCpy $LANGUAGE 1042
	${ElseIf} $DEF_LANG == "pl"
	StrCpy $LANGUAGE 1045
	${ElseIf} $DEF_LANG == "pt"
	StrCpy $LANGUAGE 1046
	${ElseIf} $DEF_LANG == "pt_BR"
	StrCpy $LANGUAGE 1046
	${ElseIf} $DEF_LANG == "ru"
	StrCpy $LANGUAGE 1049
	${ElseIf} $DEF_LANG == "tr"
	StrCpy $LANGUAGE 1055
	${ElseIf} $DEF_LANG == "uk"
	StrCpy $LANGUAGE 1058
	${ElseIf} $DEF_LANG == "zh"
	StrCpy $LANGUAGE 2052
	${ElseIf} $DEF_LANG == "zh_CN"
	StrCpy $LANGUAGE 2052
  ${EndIf}

  !insertmacro MUI_LANGDLL_DISPLAY
FunctionEnd
Function un.onInit
  !insertmacro MUI_LANGDLL_DISPLAY
FunctionEnd
Function .onInstSuccess
	${GetParameters} $1
	${StrStr} $0 $1 "/RR"
	${If} $0 != ""
		${If} ${RunningX64}
			Exec "$INSTDIR\${PRODUCT64_EXE}"
		${Else}
			Exec "$INSTDIR\${PRODUCT32_EXE}"
		${EndIf} 		
	${EndIf}
FunctionEnd
Function StartDiffractor
    ${If} ${RunningX64}
		Exec "$INSTDIR\${PRODUCT64_EXE}"
	${Else}
		Exec "$INSTDIR\${PRODUCT32_EXE}"
	${EndIf} 		
FunctionEnd

Function MyWelcomeShowCallback
	SendMessage $mui.WelcomePage.Text ${WM_SETTEXT} 0 "STR:$(WELCOME_TEXT)"
FunctionEnd

;--------------------------------
;Installer Sections

Section "Diffractor"

	Var /GLOBAL PRODUCT_EXE

	${If} ${RunningX64}
		StrCpy $PRODUCT_EXE "${PRODUCT64_EXE}"
	${Else}
		StrCpy $PRODUCT_EXE "${PRODUCT32_EXE}"
	${EndIf}  

	Push "DIFF_MAIN"
	Call .CloseProgram
    
	; Everything installed here is application data, never user data, and each release
	; expects its own copy of it. Overwriting only when newer let an update keep a stale
	; file whose timestamp happened not to be older - a stale location-places.txt silently
	; degrades place names to the legacy over-qualified form with nothing shown to the user.
	SetOverwrite on
	
	SetOutPath $INSTDIR\languages  
	File "..\exe\languages\*.po"

	SetOutPath $INSTDIR\dictionaries  
	File "..\exe\dictionaries\en_US.aff"
	File "..\exe\dictionaries\en_US.dic"

	SetOutPath $INSTDIR	
	File "..\exe\location-countries.txt"
	File "..\exe\location-places.txt"
	File "..\exe\location-states.txt"
	File "..\exe\diffractor-tools.json"

	; The installed program is LGPL-3.0-or-later, whose text and the GPL-3.0 it incorporates must
	; accompany every copy. dd.ps1 desktop stages both into exe\ before this script is compiled.
	File "..\exe\LICENSE.txt"
	File "..\exe\COPYING.GPLv3.txt"

	File "..\exe\${PRODUCT32_EXE}"
	File "..\exe\${PRODUCT64_EXE}"
		
    ${If} ${Silent}
        ; Dont add desktop icon if in silent mode
    ${Else}
		CreateShortCut "$STARTMENU\Programs\Diffractor.lnk" "$INSTDIR\$PRODUCT_EXE" "" "$INSTDIR\$PRODUCT_EXE" 0
		; Only create the desktop shortcut on a fresh install. On an update we must
		; not recreate it, otherwise a shortcut the user has deliberately deleted
		; keeps coming back after every update (issue #173).
		${If} $IS_UPDATE == "0"
			CreateShortCut "$DESKTOP\Diffractor.lnk" "$INSTDIR\$PRODUCT_EXE" "" "$INSTDIR\$PRODUCT_EXE" 0
		${EndIf}
	${EndIf}
  
	; Write the uninstall keys for Windows
	WriteRegStr SHCTX "${PRODUCT_UNINST_KEY}" "DisplayName" "${PRODUCT_NAME}"
	WriteRegStr SHCTX "${PRODUCT_UNINST_KEY}" "Publisher" "${PRODUCT_PUBLISHER}"
	WriteRegStr SHCTX "${PRODUCT_UNINST_KEY}" "DisplayIcon" "$INSTDIR\$PRODUCT_EXE"	
	WriteRegStr SHCTX "${PRODUCT_UNINST_KEY}" "URLInfoAbout" "${PRODUCT_WEB_SITE}"
	WriteRegStr SHCTX "${PRODUCT_UNINST_KEY}" "DisplayVersion" "${PRODUCT_VERSION}"
	; KB, as reported by Add/Remove Programs. Both executables plus the location data.
	WriteRegDWord SHCTX "${PRODUCT_UNINST_KEY}" "EstimatedSize" 108000
	WriteRegDWORD SHCTX "${PRODUCT_UNINST_KEY}" "NoModify" 1
	WriteRegDWORD SHCTX "${PRODUCT_UNINST_KEY}" "NoRepair" 1
	WriteRegStr SHCTX "${PRODUCT_UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
	WriteRegStr SHCTX "${PRODUCT_UNINST_KEY}" "InstallLocation" "$\"$INSTDIR$\""
	
	WriteRegStr SHCTX "${PRODUCT_SETTINGS_KEY}" "" $INSTDIR	

	; Remove the legacy context-menu entry from older versions. It was registered under
	; the generic "Folder" class, which has no default verb; adding the first static verb
	; there promoted "diffractor" to the default double-click action and hijacked normal
	; folder navigation in Explorer (folders opened in Diffractor instead of opening).
	DeleteRegKey SHCTX "Software\Classes\Folder\shell\diffractor"

	; Register the folder context-menu entry under the "Directory" class (file-system
	; folders). "Directory\shell" ships with its default verb set to "none", so this stays
	; a right-click entry only and never becomes the default double-click action.
	WriteRegStr SHCTX "Software\Classes\Directory\shell\diffractor" "" "Diffractor"
  	WriteRegStr SHCTX "Software\Classes\Directory\shell\diffractor\command" "" '"$INSTDIR\$PRODUCT_EXE" "%1"'
  	WriteRegStr SHCTX "Software\Classes\File\shell\diffractor" "" "Diffractor"
  	WriteRegStr SHCTX "Software\Classes\File\shell\diffractor\command" "" '"$INSTDIR\$PRODUCT_EXE" "%1"'

	WriteUninstaller "$INSTDIR\Uninstall.exe"

	WriteRegStr HKCU "Software\Classes\.jpe\OpenWithProgids" "${PRODUCT_NAME}" ""
	WriteRegStr HKCU "Software\Classes\.jpeg\OpenWithProgids" "${PRODUCT_NAME}" ""
	WriteRegStr HKCU "Software\Classes\.jpg\OpenWithProgids" "${PRODUCT_NAME}" ""
	WriteRegStr HKCU "Software\Classes\.png\OpenWithProgids" "${PRODUCT_NAME}" ""
	WriteRegStr HKCU "Software\Classes\.gif\OpenWithProgids" "${PRODUCT_NAME}" ""
	WriteRegStr HKCU "Software\Classes\.mp4\OpenWithProgids" "${PRODUCT_NAME}" ""
	WriteRegStr HKCU "Software\Classes\.mov\OpenWithProgids" "${PRODUCT_NAME}" ""
	WriteRegStr HKCU "Software\Classes\.avi\OpenWithProgids" "${PRODUCT_NAME}" ""
	WriteRegStr HKCU "Software\Classes\.mp3\OpenWithProgids" "${PRODUCT_NAME}" ""
	WriteRegStr HKCU "Software\Classes\.cr2\OpenWithProgids" "${PRODUCT_NAME}" ""
	WriteRegStr HKCU "Software\Classes\.cr3\OpenWithProgids" "${PRODUCT_NAME}" ""
	WriteRegStr HKCU "Software\Classes\.webp\OpenWithProgids" "${PRODUCT_NAME}" ""

	WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.jpe\OpenWithProgids" "${PRODUCT_NAME}" ""
	WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.jpeg\OpenWithProgids" "${PRODUCT_NAME}" ""
	WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.jpg\OpenWithProgids" "${PRODUCT_NAME}" ""
	WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.png\OpenWithProgids" "${PRODUCT_NAME}" ""
	WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.gif\OpenWithProgids" "${PRODUCT_NAME}" ""

	;WriteRegDWORD HKCU "Software\Classes\${PRODUCT_NAME}" "EditFlags" 65536
  
	WriteRegStr HKCU "Software\Classes\${PRODUCT_NAME}" "" "Diffractor"
	WriteRegStr HKCU "Software\Classes\${PRODUCT_NAME}" "FriendlyTypeName" "Diffractor"
	;WriteRegStr HKCU "Software\Classes\${PRODUCT_NAME}\DefaultIcon" "" "$INSTDIR\Clipboard.ico"
    WriteRegStr HKCU "Software\Classes\${PRODUCT_NAME}\shell\open\command" "" '"$INSTDIR\$PRODUCT_EXE" "%1"'

	!insertmacro UPDATEFILEASSOC

	WriteRegStr HKCU "${PRODUCT_SETTINGS_KEY}" "lang" "$(APP_LANG)"

	WriteRegStr HKCU "${PRODUCT_SETTINGS_KEY}" "install_lang" $LANGUAGE

SectionEnd

 
;--------------------------------
;Uninstaller Section

Section "Uninstall"

	; ExecWait '"taskkill" /f /IM diffractor.exe' $0

	Push "DIFF_MAIN"
	Call un.CloseProgram

	; remove registry keys
	DeleteRegKey SHCTX "${PRODUCT_UNINST_KEY}"
	; PRODUCT_SETTINGS_KEY is NOT removed. SHCTX is HKCU here (RequestExecutionLevel user), and
	; HKCU\Software\Diffractor is where the app actually keeps every setting on a desktop install -
	; collection roots, favourite searches and tags, copyright fields, import and sync paths, panel
	; layout. Deleting it is the registry half of the same data loss the file list below avoids.
	DeleteRegKey SHCTX "Software\Classes\Directory\shell\diffractor"
	; Also remove the legacy "Folder\shell" entry written by older versions, which
	; hijacked the default folder double-click action (see install section for details).
	DeleteRegKey SHCTX "Software\Classes\Folder\shell\diffractor"
	DeleteRegKey SHCTX "Software\Classes\File\shell\diffractor"

	DeleteRegValue HKCU "Software\Classes\.jpe\OpenWithProgids" "${PRODUCT_NAME}"
	DeleteRegValue HKCU "Software\Classes\.jpeg\OpenWithProgids" "${PRODUCT_NAME}"
	DeleteRegValue HKCU "Software\Classes\.jpg\OpenWithProgids" "${PRODUCT_NAME}"
	DeleteRegValue HKCU "Software\Classes\.png\OpenWithProgids" "${PRODUCT_NAME}"
	DeleteRegValue HKCU "Software\Classes\.gif\OpenWithProgids" "${PRODUCT_NAME}"
	DeleteRegValue HKCU "Software\Classes\.mp4\OpenWithProgids" "${PRODUCT_NAME}"
	DeleteRegValue HKCU "Software\Classes\.mov\OpenWithProgids" "${PRODUCT_NAME}"
	DeleteRegValue HKCU "Software\Classes\.avi\OpenWithProgids" "${PRODUCT_NAME}"
	DeleteRegValue HKCU "Software\Classes\.mp3\OpenWithProgids" "${PRODUCT_NAME}"
	DeleteRegValue HKCU "Software\Classes\.cr2\OpenWithProgids" "${PRODUCT_NAME}"
	DeleteRegValue HKCU "Software\Classes\.cr3\OpenWithProgids" "${PRODUCT_NAME}"
	DeleteRegValue HKCU "Software\Classes\.webp\OpenWithProgids" "${PRODUCT_NAME}"

	; The install also advertises Diffractor in Explorer's per-extension Open With list.
	; Without this it survives uninstall and keeps offering an application that is gone.
	DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.jpe\OpenWithProgids" "${PRODUCT_NAME}"
	DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.jpeg\OpenWithProgids" "${PRODUCT_NAME}"
	DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.jpg\OpenWithProgids" "${PRODUCT_NAME}"
	DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.png\OpenWithProgids" "${PRODUCT_NAME}"
	DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.gif\OpenWithProgids" "${PRODUCT_NAME}"

	DeleteRegKey HKCU "Software\Classes\${PRODUCT_NAME}"

	;DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}"
	;DeleteRegKey HKCU "Software\${PRODUCT_NAME}"

	!insertmacro UPDATEFILEASSOC
	
	; $INSTDIR is $LOCALAPPDATA\Diffractor, which is also where the app keeps the index database,
	; the settings file, the map tile cache and any dictionary the user downloaded. A recursive
	; delete of the whole folder therefore destroys the user's data along with the program, with no
	; prompt and no way back. Only what the installer wrote is removed, plus the app's own logs, and
	; the folders go only if they are empty afterwards.
	Delete /REBOOTOK "$INSTDIR\${PRODUCT32_EXE}"
	Delete /REBOOTOK "$INSTDIR\${PRODUCT64_EXE}"
	Delete "$INSTDIR\location-countries.txt"
	Delete "$INSTDIR\location-places.txt"
	Delete "$INSTDIR\location-states.txt"
	Delete "$INSTDIR\diffractor-tools.json"
	Delete "$INSTDIR\LICENSE.txt"
	Delete "$INSTDIR\COPYING.GPLv3.txt"
	Delete "$INSTDIR\diffractor.log"
	Delete "$INSTDIR\diffractor.previous.log"
	Delete "$INSTDIR\languages\*.po"
	RMDir "$INSTDIR\languages"
	; Only the two shipped dictionary files: any other file here was downloaded by the user.
	Delete "$INSTDIR\dictionaries\en_US.aff"
	Delete "$INSTDIR\dictionaries\en_US.dic"
	RMDir "$INSTDIR\dictionaries"
	Delete $INSTDIR\uninstall.exe
	Delete "$STARTMENU\Programs\Diffractor.lnk"
	Delete "$DESKTOP\Diffractor.lnk"

	; Expected to fail while the user's data is still here, which is the point. /REBOOTOK must not be
	; used: this folder is not empty by design, and the flag would both schedule it for deletion on
	; reboot and end every uninstall on the "restart required" page.
	RMDir "$INSTDIR"

SectionEnd  
