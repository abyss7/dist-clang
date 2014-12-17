set gn_path="%~dp0..\bin\win\gn.exe"

%gn_path% --check --args="config_for_debug=true %*" gen //out/Debug.gn
%gn_path% --check --args="%*" gen //out/Release.gn
%gn_path% --check --args="config_for_tests=true %*" gen //out/Test.gn
