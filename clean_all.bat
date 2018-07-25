del %cd%\*.tlog /s/q
del %cd%\*.ipch /s/q
del %cd%\*.pdb /s/q
del %cd%\*.bak /s/q
del %cd%\*.sdf /s/q
del %cd%\*.suo /s/q
del %cd%\*.ncb /s/q
del %cd%\*.pch /s/q
del %cd%\*.idb /s/q
FOR /F "tokens=*" %%G IN ('DIR /B /AD /S *ipch') DO echo "%%G" && rd /Q /S  "%%G"