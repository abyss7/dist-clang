set gn_path="%~dp0..\bin\win\gn.exe"

%gn_path% --check --args="config_for_debug=true %*" gen --ide=qtcreator //out/Debug.gn
%gn_path% --check --args="%*" gen --ide=qtcreator //out/Release.gn
%gn_path% --check --args="config_for_tests=true %*" gen --ide=qtcreator --root-target=Tests //out/Test.gn
