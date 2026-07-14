# USB Authentication Recovery Design

## Goal

Recover a forgotten administrator password on a physically connected K10 without
erasing pump calibration, pH calibration, method settings, or Wi-Fi settings.

## Design

The normal firmware remains unchanged in normal builds. A separate
`unihiker-auth-recovery` PlatformIO environment compiles a minimal recovery
application instead of the titrator application. Its only persistent write is a
fresh administrator credential in the `auth` Preferences namespace.

The new administrator password is converted on the host into an ignored local
header containing PBKDF2 salt and hash only. The recovery image writes those
credential fields using `AuthStore::saveAdministrator`, then waits for the
operator to reflash the normal `unihiker` firmware. Neither the password nor its
derived material is committed, printed, returned through HTTP, or written to the
serial log.

## Safety and failure behavior

- The recovery image does not initialize or command either pump.
- It does not clear the `cal`, Wi-Fi, method, or other Preferences namespaces.
- A failed credential write reports failure over serial and does not claim
  recovery.
- The normal firmware must be flashed immediately after a successful recovery
  image run; recovery mode offers no Web control surface.

## Verification

- A source-level protocol test asserts that recovery writes through the existing
  atomic administrator credential path and that its environment is explicitly
  selected.
- Build both normal and recovery PlatformIO environments.
- Flash recovery over USB, observe its serial success marker, flash normal
  firmware, then verify the new administrator password can create a session.
