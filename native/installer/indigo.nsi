; Instalador do Índigo para Windows (NSIS). Instala só para o usuário (sem pedir administrador).
; Gerado por native/tools/pacote-pc.sh:  makensis -DVERSAO=0.9.0 -DEXE=caminho\indigo.exe -DSAIDA=Indigo-0.9.0-instalador.exe indigo.nsi
; Os saves ficam em %APPDATA%\Indigo e NÃO são apagados ao desinstalar.

Unicode true
Target amd64-unicode  ; o Índigo é 64 bits; assim o instalador também é
!ifndef VERSAO
  !define VERSAO "0.0.0"
!endif
!define PROGID "Indigo.Jogo"
!define UNINST "Software\Microsoft\Windows\CurrentVersion\Uninstall\Indigo"

Name "Índigo ${VERSAO}"
OutFile "${SAIDA}"
RequestExecutionLevel user
InstallDir "$LOCALAPPDATA\Programs\Indigo"
InstallDirRegKey HKCU "Software\Indigo" "Pasta"
Icon "..\res\indigo.ico"
UninstallIcon "..\res\indigo.ico"
SetCompressor /SOLID lzma
VIProductVersion "${VERSAO}.0"
VIAddVersionKey /LANG=1046 "ProductName" "Indigo"
VIAddVersionKey /LANG=1046 "FileDescription" "Instalador do Indigo"
VIAddVersionKey /LANG=1046 "FileVersion" "${VERSAO}"
VIAddVersionKey /LANG=1046 "ProductVersion" "${VERSAO}"
VIAddVersionKey /LANG=1046 "LegalCopyright" "Joao Carlos Bueno Abitante"

!include "MUI2.nsh"
!define MUI_ICON "..\res\indigo.ico"
!define MUI_UNICON "..\res\indigo.ico"
!define MUI_FINISHPAGE_RUN "$INSTDIR\indigo.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Abrir o Índigo agora"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "PortugueseBR"

!macro Associa EXT
  WriteRegStr HKCU "Software\Classes\${EXT}\OpenWithProgids" "${PROGID}" ""
  ReadRegStr $0 HKCU "Software\Classes\${EXT}" ""
  StrCmp $0 "" 0 +2
    WriteRegStr HKCU "Software\Classes\${EXT}" "" "${PROGID}"
!macroend
!macro Desassocia EXT
  DeleteRegValue HKCU "Software\Classes\${EXT}\OpenWithProgids" "${PROGID}"
  ReadRegStr $0 HKCU "Software\Classes\${EXT}" ""
  StrCmp $0 "${PROGID}" 0 +2
    DeleteRegValue HKCU "Software\Classes\${EXT}" ""
!macroend

Section "Índigo (obrigatório)" SecApp
  SectionIn RO
  SetOutPath "$INSTDIR"
  File "/oname=indigo.exe" "${EXE}"
  File "/oname=LEIA-ME.txt" "${LEIAME}"
  ; licença (GPL-3.0) e créditos de terceiros vão junto com o programa, como pede a licença
  File "/oname=LICENCA.txt" "${LICENCA}"
  File "/oname=TERCEIROS.txt" "${TERCEIROS}"
  WriteUninstaller "$INSTDIR\desinstalar.exe"
  WriteRegStr HKCU "Software\Indigo" "Pasta" "$INSTDIR"
  WriteRegStr HKCU "${UNINST}" "DisplayName" "Índigo"
  WriteRegStr HKCU "${UNINST}" "DisplayVersion" "${VERSAO}"
  WriteRegStr HKCU "${UNINST}" "Publisher" "João Carlos Bueno Abitante"
  WriteRegStr HKCU "${UNINST}" "DisplayIcon" "$INSTDIR\indigo.exe"
  WriteRegStr HKCU "${UNINST}" "UninstallString" '"$INSTDIR\desinstalar.exe"'
  WriteRegStr HKCU "${UNINST}" "InstallLocation" "$INSTDIR"
  WriteRegDWORD HKCU "${UNINST}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST}" "NoRepair" 1
  CreateShortcut "$SMPROGRAMS\Índigo.lnk" "$INSTDIR\indigo.exe"
SectionEnd

Section "Atalho na área de trabalho" SecDesk
  CreateShortcut "$DESKTOP\Índigo.lnk" "$INSTDIR\indigo.exe"
SectionEnd

Section "Abrir jogos com dois cliques (.nes .gb .gbc .gba .gg .sms .ngp .ngc)" SecAssoc
  WriteRegStr HKCU "Software\Classes\${PROGID}" "" "Jogo do Índigo"
  WriteRegStr HKCU "Software\Classes\${PROGID}\DefaultIcon" "" '"$INSTDIR\indigo.exe",0'
  WriteRegStr HKCU "Software\Classes\${PROGID}\shell\open\command" "" '"$INSTDIR\indigo.exe" "%1"'
  !insertmacro Associa ".nes"
  !insertmacro Associa ".gb"
  !insertmacro Associa ".gbc"
  !insertmacro Associa ".gba"
  !insertmacro Associa ".gg"
  !insertmacro Associa ".sms"
  !insertmacro Associa ".ngp"
  !insertmacro Associa ".ngc"
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
SectionEnd

Section "Uninstall"
  !insertmacro Desassocia ".nes"
  !insertmacro Desassocia ".gb"
  !insertmacro Desassocia ".gbc"
  !insertmacro Desassocia ".gba"
  !insertmacro Desassocia ".gg"
  !insertmacro Desassocia ".sms"
  !insertmacro Desassocia ".ngp"
  !insertmacro Desassocia ".ngc"
  DeleteRegKey HKCU "Software\Classes\${PROGID}"
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
  Delete "$SMPROGRAMS\Índigo.lnk"
  Delete "$DESKTOP\Índigo.lnk"
  Delete "$INSTDIR\indigo.exe"
  Delete "$INSTDIR\LEIA-ME.txt"
  Delete "$INSTDIR\LICENCA.txt"
  Delete "$INSTDIR\TERCEIROS.txt"
  Delete "$INSTDIR\desinstalar.exe"
  RMDir "$INSTDIR"
  DeleteRegKey HKCU "${UNINST}"
  DeleteRegKey HKCU "Software\Indigo"
SectionEnd
