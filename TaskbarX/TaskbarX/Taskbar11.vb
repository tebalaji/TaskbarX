Option Strict On

Imports System.IO
Imports System.Runtime.InteropServices
Imports System.Text
Imports System.Threading
Imports Microsoft.Win32

'Windows 11 support.
'The Windows 11 taskbar is drawn with XAML inside explorer.exe, so there is no MSTaskListWClass to move.
'Instead TaskbarX loads TaskbarX11Hook.dll into explorer.exe, which moves the XAML buttons itself
'(Start to the left, app icons centered, Widgets hidden). Its settings are passed through the registry.
Public Class Taskbar11

    Private Const SettingsKey As String = "Software\TaskbarX\Win11"
    Private Const HookDll As String = "TaskbarX11Hook.dll"
    Private Const WH_CALLWNDPROC As Integer = 4

    <DllImport("kernel32.dll", CharSet:=CharSet.Unicode, SetLastError:=True)>
    Private Shared Function LoadLibrary(ByVal lpFileName As String) As IntPtr
    End Function

    <DllImport("kernel32.dll", CharSet:=CharSet.Ansi, ExactSpelling:=True)>
    Private Shared Function GetProcAddress(ByVal hModule As IntPtr, ByVal procName As String) As IntPtr
    End Function

    <DllImport("user32.dll", SetLastError:=True)>
    Private Shared Function SetWindowsHookEx(ByVal idHook As Integer, ByVal lpfn As IntPtr, ByVal hMod As IntPtr, ByVal dwThreadId As UInteger) As IntPtr
    End Function

    <DllImport("user32.dll")>
    Private Shared Function UnhookWindowsHookEx(ByVal hhk As IntPtr) As Boolean
    End Function

    <DllImport("user32.dll", CharSet:=CharSet.Unicode)>
    Private Shared Function RegisterWindowMessage(ByVal lpString As String) As UInteger
    End Function

    <DllImport("user32.dll")>
    Private Shared Function SendMessageTimeout(ByVal hWnd As IntPtr, ByVal msg As UInteger, ByVal wParam As IntPtr, ByVal lParam As IntPtr, ByVal flags As UInteger, ByVal timeout As UInteger, ByRef result As IntPtr) As IntPtr
    End Function

    <DllImport("user32.dll")>
    Private Shared Function GetWindowThreadProcessId(ByVal hWnd As IntPtr, ByRef lpdwProcessId As UInteger) As UInteger
    End Function

    <DllImport("user32.dll")>
    Private Shared Function EnumChildWindows(ByVal hWndParent As IntPtr, ByVal lpEnumFunc As EnumChildProc, ByVal lParam As IntPtr) As Boolean
    End Function

    Private Delegate Function EnumChildProc(ByVal hwnd As IntPtr, ByVal lParam As IntPtr) As Boolean

    Public Shared Function WindowsBuild() As Integer
        Try
            Using key = Registry.LocalMachine.OpenSubKey("SOFTWARE\Microsoft\Windows NT\CurrentVersion")
                Return CInt(key.GetValue("CurrentBuildNumber", "0"))
            End Using
        Catch
            Return 0
        End Try
    End Function

    'True when Shell_TrayWnd is the Windows 11 XAML taskbar (and not e.g. the classic taskbar restored by ExplorerPatcher).
    Public Shared Function IsXamlTaskbar(ByVal Shell_TrayWnd As IntPtr) As Boolean
        If Shell_TrayWnd = IntPtr.Zero OrElse WindowsBuild() < 22000 Then
            Return False
        End If

        Dim found As Boolean = False
        EnumChildWindows(Shell_TrayWnd, Function(hwnd, lParam)
                                            Dim sClassName As New StringBuilder("", 256)
                                            Win32.GetClassName(hwnd, sClassName, 256)
                                            If sClassName.ToString = "Windows.UI.Composition.DesktopWindowContentBridge" Then
                                                found = True
                                                Return False
                                            End If
                                            Return True
                                        End Function, IntPtr.Zero)
        Return found
    End Function

    Private Const ExplorerAdvancedKey As String = "Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced"
    Private Const HWND_BROADCAST As Integer = &HFFFF
    Private Const WM_SETTINGCHANGE As UInteger = &H1A

    <DllImport("user32.dll", CharSet:=CharSet.Unicode)>
    Private Shared Function SendMessageTimeout(ByVal hWnd As IntPtr, ByVal msg As UInteger, ByVal wParam As IntPtr, ByVal lParam As String, ByVal flags As UInteger, ByVal timeout As UInteger, ByRef result As IntPtr) As IntPtr
    End Function

    'Tells explorer to re-read the taskbar settings, like the Settings app does.
    Private Shared Sub NotifyTaskbarSettingsChanged()
        Dim result As IntPtr
        SendMessageTimeout(New IntPtr(HWND_BROADCAST), WM_SETTINGCHANGE, IntPtr.Zero, "TraySettings", 2, 1000, result)
    End Sub

    'Start on the left: switch Windows to its own left-aligned taskbar (Settings > Personalization > Taskbar >
    'Taskbar alignment), so Start, Search and Task View really are on the left. Moving them visually is not enough:
    'the Start menu, the open search box and its popup are placed by Windows at the buttons' real positions.
    'TaskbarX then only moves the app icons to the center. The previous alignment is saved for Stop.
    Private Shared Sub ApplyAlignment()
        Using advanced = Registry.CurrentUser.CreateSubKey(ExplorerAdvancedKey)
            Using key = Registry.CurrentUser.CreateSubKey(SettingsKey)
                Dim current = advanced.GetValue("TaskbarAl")
                If current IsNot Nothing AndAlso CInt(current) = 0 Then
                    Return
                End If
                If key.GetValue("SavedTaskbarAl") Is Nothing Then
                    '-1 = the value didn't exist (Windows' default, centered)
                    key.SetValue("SavedTaskbarAl", If(current Is Nothing, -1, CInt(current)), RegistryValueKind.DWord)
                End If
                advanced.SetValue("TaskbarAl", 0, RegistryValueKind.DWord)
            End Using
        End Using
        NotifyTaskbarSettingsChanged()
    End Sub

    'Puts back the taskbar alignment from before ApplyAlignment.
    Private Shared Sub RestoreAlignment()
        Using key = Registry.CurrentUser.OpenSubKey(SettingsKey, True)
            Dim saved = key?.GetValue("SavedTaskbarAl")
            If saved Is Nothing Then
                Return
            End If
            Using advanced = Registry.CurrentUser.CreateSubKey(ExplorerAdvancedKey)
                If CInt(saved) = -1 Then
                    advanced.DeleteValue("TaskbarAl", False)
                Else
                    advanced.SetValue("TaskbarAl", CInt(saved), RegistryValueKind.DWord)
                End If
            End Using
            key.DeleteValue("SavedTaskbarAl", False)
        End Using
        NotifyTaskbarSettingsChanged()
    End Sub

    Public Shared Sub WriteSettings()
        If Settings.Win11StartOnLeft = 1 Then
            ApplyAlignment()
        Else
            RestoreAlignment()
        End If

        Using key = Registry.CurrentUser.CreateSubKey(SettingsKey)
            key.SetValue("Active", 1, RegistryValueKind.DWord)
            key.SetValue("StartOnLeft", Settings.Win11StartOnLeft, RegistryValueKind.DWord)
            key.SetValue("KeepButtonsLeft", Settings.Win11KeepButtonsLeft, RegistryValueKind.DWord)
            key.SetValue("HideWidgets", Settings.Win11HideWidgets, RegistryValueKind.DWord)
            key.SetValue("StartPadding", Settings.Win11StartPadding, RegistryValueKind.DWord)
            key.SetValue("CenterIcons", If(Settings.DontCenterTaskbar = 1, 0, 1), RegistryValueKind.DWord)
            key.SetValue("CenterInBetween", Settings.CenterInBetween, RegistryValueKind.DWord)
            key.SetValue("Offset", Settings.PrimaryTaskbarOffset, RegistryValueKind.DWord)
            key.SetValue("TaskbarStyle", Settings.TaskbarStyle, RegistryValueKind.DWord)
            key.SetValue("StartMenuStyle", Settings.Win11StartMenuStyle, RegistryValueKind.DWord)
            'ARGB (little-endian bytes B, G, R, A); the -color alpha is a percentage
            Dim alpha = CByte(Math.Max(0, Math.Min(100, Settings.TaskbarStyleAlpha)) * 2.55)
            Dim argb = BitConverter.ToInt32(New Byte() {CByte(Settings.TaskbarStyleBlue), CByte(Settings.TaskbarStyleGreen), CByte(Settings.TaskbarStyleRed), alpha}, 0)
            key.SetValue("StyleColor", argb, RegistryValueKind.DWord)
            key.SetValue("AnimationSpeed", Settings.AnimationSpeed, RegistryValueKind.DWord)
            key.SetValue("AnimationStyle", Settings.AnimationStyle, RegistryValueKind.String)
            key.SetValue("Debug", Settings.Win11Debug, RegistryValueKind.DWord)
        End Using
    End Sub

    'Tells a hook that is already loaded to re-read its settings.
    Private Shared Sub SignalReload()
        Dim reload As EventWaitHandle = Nothing
        If EventWaitHandle.TryOpenExisting("Local\TaskbarX11Hook_Reload", reload) Then
            reload.Set()
            reload.Dispose()
        End If
    End Sub

    Private Shared Function IsHookLoaded(ByVal explorerPid As UInteger) As Boolean
        Dim active As Mutex = Nothing
        If Mutex.TryOpenExisting("Local\TaskbarX11Hook_Active_" & explorerPid, active) Then
            active.Dispose()
            Return True
        End If
        Return False
    End Function

    Private Shared Function Inject(ByVal Shell_TrayWnd As IntPtr, ByVal threadId As UInteger, ByVal explorerPid As UInteger) As Boolean
        If IsHookLoaded(explorerPid) Then
            SignalReload()
            Return True
        End If

        If Not Environment.Is64BitProcess AndAlso Environment.Is64BitOperatingSystem Then
            Console.WriteLine("Windows 11 support needs the 64-bit build of TaskbarX.")
            Return False
        End If

        Dim dllPath As String = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, HookDll)
        If Not File.Exists(dllPath) Then
            Console.WriteLine(HookDll & " is missing next to TaskbarX.exe.")
            Return False
        End If

        Dim hModule As IntPtr = LoadLibrary(dllPath)
        Dim hookProc As IntPtr = GetProcAddress(hModule, "TaskbarX11HookProc")
        If hookProc = IntPtr.Zero Then
            Console.WriteLine("Could not load " & dllPath)
            Return False
        End If

        'The hook gets the DLL into explorer.exe; the DLL then attaches itself to the taskbar's XAML and pins itself,
        'so the hook can be removed again right after.
        Dim hook As IntPtr = SetWindowsHookEx(WH_CALLWNDPROC, hookProc, hModule, threadId)
        If hook = IntPtr.Zero Then
            Console.WriteLine("SetWindowsHookEx failed: " & Marshal.GetLastWin32Error())
            Return False
        End If

        Try
            Dim result As IntPtr
            SendMessageTimeout(Shell_TrayWnd, RegisterWindowMessage("TaskbarX11Hook_Inject"), IntPtr.Zero, IntPtr.Zero, 2, 5000, result)

            For i = 1 To 60
                If IsHookLoaded(explorerPid) Then
                    Console.WriteLine("TaskbarX11Hook loaded into explorer.exe (" & explorerPid & ")")
                    Return True
                End If
                Thread.Sleep(250)
            Next
            Return False
        Finally
            UnhookWindowsHookEx(hook)
        End Try
    End Function

    <DllImport("user32.dll")>
    Private Shared Function PostMessage(ByVal hWnd As IntPtr, ByVal msg As UInteger, ByVal wParam As IntPtr, ByVal lParam As IntPtr) As Boolean
    End Function

    <DllImport("user32.dll", CharSet:=CharSet.Unicode)>
    Private Shared Function GetProp(ByVal hWnd As IntPtr, ByVal lpString As String) As IntPtr
    End Function

    Private Const WH_GETMESSAGE As Integer = 3
    Private Shared startMenuHook As IntPtr = IntPtr.Zero
    Private Shared startMenuHookWindow As IntPtr = IntPtr.Zero

    'The Start menu's window (a CoreWindow of StartMenuExperienceHost.exe). Found by process, not by its title,
    'which is translated on other Windows languages.
    Private Shared Function FindStartMenuWindow() As IntPtr
        Dim startMenuPids = Process.GetProcessesByName("StartMenuExperienceHost").Select(Function(p) CUInt(p.Id)).ToList()
        If startMenuPids.Count = 0 Then
            Return IntPtr.Zero
        End If
        'EnumWindows skips the windows of Store apps like this one; FindWindowEx doesn't.
        Dim hwnd As IntPtr = IntPtr.Zero
        Do
            hwnd = Win32.FindWindowEx(IntPtr.Zero, hwnd, "Windows.UI.Core.CoreWindow", Nothing)
            If hwnd = IntPtr.Zero Then
                Return IntPtr.Zero
            End If
            Dim pid As UInteger
            GetWindowThreadProcessId(hwnd, pid)
            If startMenuPids.Contains(pid) Then
                Return hwnd
            End If
        Loop
    End Function

    'Loads the hook into the Start menu so it can apply the taskbar style there as well. Windows suspends the
    'Start menu app while it's closed, so the inject message is posted and the hook stays installed until the
    'Start menu has been opened and the DLL marked its window as loaded.
    Private Shared Sub InjectStartMenu()
        Dim startWindow = FindStartMenuWindow()
        If startWindow = IntPtr.Zero Then
            Return
        End If

        If Not GetProp(startWindow, "TaskbarX11Hook") = IntPtr.Zero Then
            If Not startMenuHook = IntPtr.Zero Then
                UnhookWindowsHookEx(startMenuHook)
                startMenuHook = IntPtr.Zero
                Console.WriteLine("TaskbarX11Hook loaded into the Start menu")
            End If
            Return
        End If

        If Not startMenuHook = IntPtr.Zero Then
            If startMenuHookWindow = startWindow Then
                Return 'still waiting for the Start menu to be opened
            End If
            UnhookWindowsHookEx(startMenuHook) 'the Start menu app restarted
            startMenuHook = IntPtr.Zero
        End If

        Dim dllPath As String = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, HookDll)
        Dim hModule As IntPtr = LoadLibrary(dllPath)
        Dim hookProc As IntPtr = GetProcAddress(hModule, "TaskbarX11GetMsgProc")
        If hookProc = IntPtr.Zero Then
            Return
        End If

        Dim startMenuPid As UInteger
        Dim threadId = GetWindowThreadProcessId(startWindow, startMenuPid)
        startMenuHook = SetWindowsHookEx(WH_GETMESSAGE, hookProc, hModule, threadId)
        startMenuHookWindow = startWindow
        If Not startMenuHook = IntPtr.Zero Then
            PostMessage(startWindow, RegisterWindowMessage("TaskbarX11Hook_Inject"), IntPtr.Zero, IntPtr.Zero)
        End If
    End Sub

    'Keeps the hook loaded for as long as TaskbarX runs, also after explorer.exe restarts.
    Public Shared Sub Run()
        Dim injectedPid As UInteger = 0
        Do
            Try
                Dim Shell_TrayWnd = Win32.FindWindowByClass("Shell_TrayWnd", CType(0, IntPtr))
                If Not Shell_TrayWnd = IntPtr.Zero Then
                    Dim explorerPid As UInteger
                    Dim threadId = GetWindowThreadProcessId(Shell_TrayWnd, explorerPid)
                    If Not explorerPid = injectedPid AndAlso Inject(Shell_TrayWnd, threadId, explorerPid) Then
                        injectedPid = explorerPid
                    End If
                End If
                If Settings.Win11StartMenuStyle = 1 AndAlso Not Settings.TaskbarStyle = 0 Then
                    InjectStartMenu()
                End If
            Catch ex As Exception
                Console.WriteLine(ex.Message)
            End Try
            Thread.Sleep(2000)
        Loop
    End Sub

    'True when the hook is loaded in the running explorer.exe and currently moving the taskbar's buttons.
    Public Shared Function IsOn() As Boolean
        Try
            Using key = Registry.CurrentUser.OpenSubKey(SettingsKey)
                If key Is Nothing OrElse Not CInt(key.GetValue("Active", 0)) = 1 Then
                    Return False
                End If
            End Using
            Dim Shell_TrayWnd = Win32.FindWindowByClass("Shell_TrayWnd", CType(0, IntPtr))
            Dim explorerPid As UInteger
            GetWindowThreadProcessId(Shell_TrayWnd, explorerPid)
            Return explorerPid <> 0 AndAlso IsHookLoaded(explorerPid)
        Catch
            Return False
        End Try
    End Function

    'Puts the taskbar back the way Windows draws it. The hook stays loaded but does nothing until TaskbarX starts again.
    Public Shared Sub [Stop]()
        Try
            Using key = Registry.CurrentUser.OpenSubKey(SettingsKey, True)
                If key Is Nothing Then
                    Return 'never ran on Windows 11
                End If
                key.SetValue("Active", 0, RegistryValueKind.DWord)
            End Using
            SignalReload()
            RestoreAlignment()
        Catch ex As Exception
            Console.WriteLine(ex.Message)
        End Try
    End Sub

End Class
