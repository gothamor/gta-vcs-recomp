// VCS disc streaming without the IOP (docs/NATIVE_PORT_ROADMAP.md milestone 6 step 1).
#pragma once

class PS2Runtime;

namespace vcs
{
    // The EE side of CDSTREAM.IRX's stream server (SIF RPC 0x65686577): read / poll / fn 5 replaced by reads from the
    // disc image on a host thread straight into EE RAM. VCS_NATIVE_STREAM=0 keeps the IOP's (the reference).
    void installStreamHooks(PS2Runtime &runtime);
}
