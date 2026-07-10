# Repository agent instructions

## Windows VRR builds and deployment

- Any time an agent successfully builds the Windows Moonlight application in this workspace—whether the build is incremental, a verification build, or a release build—the agent must immediately deploy the binaries produced by that exact build to `C:\Users\Chase\Desktop\VRR`.
- A build task is not complete until this deployment has succeeded. At minimum, replace `Moonlight.exe`. Also replace any runtime `.dll` rebuilt as part of the same build when its contents changed (for example, `AntiHooking.dll`).
- Never replace or delete portable data, settings, logs, dumps, cache, box art, or other user-generated files in the Desktop VRR folder. Do not mirror or clean the destination directory.
- After copying, verify that each deployed binary has the same SHA-256 hash as its build output and report the deployed path and timestamp.
- If a destination binary is locked by a running Moonlight process, do not terminate the process without permission. Report that Moonlight must be closed, then finish the copy and verification once it is available.
