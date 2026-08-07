param([Parameter(ValueFromRemainingArguments=$true)]$Args)
if ($Args.Count -gt 0 -and $Args[0] -match 'ccache') { $Args = $Args[1..($Args.Length-1)] }
& $Args[0] @($Args[1..($Args.Length-1)])
