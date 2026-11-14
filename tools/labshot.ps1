param([string]$Url, [string]$Out, [int]$Wait = 7, [string]$Js = "", [int]$After = 3, [string]$Size = "1600,1000")
$edge = "C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
$prof = Join-Path $env:TEMP "labshot-profile"
$p = Start-Process $edge -ArgumentList @("--headless=new", "--disable-gpu", "--hide-scrollbars", "--remote-debugging-port=9333",
    "--user-data-dir=$prof", "--no-first-run", "--disable-sync", "--no-default-browser-check", "--window-size=$Size", $Url) -PassThru
function Send($ws, [int]$id, [string]$json) {
  $msg = [Text.Encoding]::UTF8.GetBytes($json)
  $ws.SendAsync([ArraySegment[byte]]$msg, "Text", $true, [Threading.CancellationToken]::None).Wait()
  $buf = New-Object byte[] 1048576
  while ($true) {
    $ms = New-Object IO.MemoryStream
    do { $r = $ws.ReceiveAsync([ArraySegment[byte]]$buf, [Threading.CancellationToken]::None).Result; $ms.Write($buf, 0, $r.Count) } until ($r.EndOfMessage)
    $o = [Text.Encoding]::UTF8.GetString($ms.ToArray()) | ConvertFrom-Json
    if ($o.id -eq $id) { return $o }
  }
}
try {
  Start-Sleep -Seconds $Wait
  $targets = Invoke-RestMethod "http://127.0.0.1:9333/json"
  $ws_url = ($targets | Where-Object { $_.type -eq "page" -and $_.url -like "$Url*" } | Select-Object -First 1).webSocketDebuggerUrl
  $ws = New-Object System.Net.WebSockets.ClientWebSocket
  $ws.ConnectAsync([Uri]$ws_url, [Threading.CancellationToken]::None).Wait()
  if ($Js) {
    $expr = $Js | ConvertTo-Json
    $res = Send $ws 1 ('{"id":1,"method":"Runtime.evaluate","params":{"expression":' + $expr + ',"awaitPromise":true}}')
    if ($res.result.exceptionDetails) { "JS error: " + ($res.result.exceptionDetails | ConvertTo-Json -Depth 5 -Compress) }
    Start-Sleep -Seconds $After
  }
  $res = Send $ws 2 '{"id":2,"method":"Runtime.evaluate","params":{"expression":"JSON.stringify(window.__errors||[])"}}'
  "page errors: " + $res.result.result.value
  $shot = Send $ws 3 '{"id":3,"method":"Page.captureScreenshot","params":{"format":"png"}}'
  [IO.File]::WriteAllBytes($Out, [Convert]::FromBase64String($shot.result.data))
  "saved $Out"
} finally {
  Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
  Get-Process msedge -ErrorAction SilentlyContinue | Where-Object { $_.CommandLine -like "*labshot-profile*" } | Stop-Process -Force -ErrorAction SilentlyContinue
}
