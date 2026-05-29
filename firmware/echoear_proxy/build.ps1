$env:Path = "C:\esp\esp-idf\tools;C:\Python313;C:\Python313\Scripts;" + $env:Path
. C:\esp\esp-idf\export.ps1
idf.py build
