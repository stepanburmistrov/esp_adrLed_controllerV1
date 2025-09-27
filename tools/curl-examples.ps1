param([string]$base = "http://ledcontroller.local")
Invoke-WebRequest "$base/on"
Invoke-WebRequest "$base/api/color?hex=00FF80"
Invoke-WebRequest "$base/api/brightness?value=200"
Invoke-WebRequest "$base/api/state"
