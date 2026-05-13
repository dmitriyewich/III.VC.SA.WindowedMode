## 2.2
- **Два окна San Andreas (US 1.0):** вместе с оконным режимом можно запускать **вторую копию** игры — она не должна зависать сразу после главного меню, как без этих доработок.
- **Связка обходов «одна копия»:** не один шаг, а **полный набор** мелких правок в духе известного мода «несколько процессов» для SA, чтобы вторая копия не упиралась в оставшиеся проверки.
- **Своё имя класса окна у каждой копии:** Windows не путает два окна одной и той же игры при регистрации класса.
- **Своя привязка картинки к окну:** у каждой копии графика видит **своё** окно, а не «чужое».
- **Mod Loader:** если используется **Mod Loader**, дополнительные правки для второй копии **отключаются** — они с ним не совместимы; плагин ведёт себя ближе к обычному оригиналу.
- **CI:** добавлен GitHub Actions workflow сборки **Release Win32** (артефакт `III.VC.SA.WindowedMode.asi`, загрузка при создании Release).

## 2.0
- added error message about unsupported game version
- added error message about missing ASI Loader
- added **auto pause** and **auto resume** features
- removed CoordsManager menu
- simplified settings: Alt+Enter now cycles between all available window styles
- window size and style are now stored in the config file and restored on game start
- proper support of multiple monitors
- always show cursor over the inactive game window
- fixed cursor moving out of the window during gameplay in GTA3 and GTA-VC
- fixed flickering window title text
- added aspect ratio information to window title text
- added snapping to known aspec ratios when resizing the window
- fixed missing window icon in GTA3
- fixed issues with audio and animations in the main menu by implementing a frame limiter for it
- fixed freezes when **Grinch's ImGUI** plugin is used

## 1.17
- fixed incorrect positioning of the window in full mode introduced in 1.16
- always show cursor over inactive game window

## 1.16
- setting of initial window position is now done on game start, not postponed until entering main menu
- last window position is now stored in config file and restored on game start

## 1.15
- fixed mouse cursor not showing up in GTA SA
