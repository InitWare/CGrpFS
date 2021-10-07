CGrpFS
======

CGrpFS is a tiny implementation of the GNU/Linux CGroup filesystem for BSD
platforms. It takes the form of a either a PUFFS or FUSE filesystem, and
implements robust tracking of processes. Resource control, however, is not
present; the different BSD platforms each provide different mechanisms for this,
none of which are trivially adapted to CGroups semantics. The process tracking
alone is sufficient for the main user of CGrpFS,
[InitWare](https://github.com/InitWare/InitWare), a service manager derived from
systemd.

CGrpFS is available under the Modified BSD Licence. It is not as-yet very well
tested, but seems to work fine for InitWare's purposes.

Architecture
------

CGrpFS was implemented quickly and is not necessarily the most efficient in
design.

Process tracking is implemented with the process filter for Kernel Queues. This
provides CGrpFS with notifications whenever a process forks or exits to which it
has attached a filter. On all BSD platforms except macOS, the filter is
automatically applied to all the transitive subprocesses spawned by a process
after the filter is attached. A filter is attached as soon as a PID is added to
a CGroup, so the Linux semantics are matched.

For simplicity, all the files and directories of the CGroup filesystem are
backed by node structures, which are akin to a combination of an `inode` and
`dirent` structure. These nodes are hierarchically ordered and each stores a
name, `stat` structure, a type (CGroup directory, `cgroup.procs` file, ...) and
type-specific data. A CGroup directory node, for example, stores a linked list
of all PIDs within it. It might be better to take an approach that maintains
less data, but bear in mind that at least permissions data must be stored
for nodes, as the GNU/Linux CGroup filesystem allows changing permissions, e.g.
to facilitate delegation.

To try to ensure consistency of file contents over the course of multiple reads,
each `open` operation in the FUSE version of CGrpFS allocates a buffer into
which the contents of the associated file is generated in full, and this buffer
is used for each read with that FUSE file handle. This may not work properly in
every case because the SunOS VFS (as imitated by BSD) enforces a distinction
between the file and vnode levels absent from GNU/Linux. The likely result of
this distinction is that read operations may not be mapped to the right file
handle during read operations. The only viable fix (which would also work for
PUFFS) would be the generation of a fresh vnode for every open.

A mini-ProcFS is also provided with only a minimal `cgroup` file present in each
PID's directory. The nodes for directories (and the contained `cgroup` file)
within that hierarchy are generated dynamically in response to getattr() events
to eliminate the need to preallocate the entire lot, and these might feasibly be
pruned if unused for some time. Their purpose is to allow InitWare to determine
the containing CGroup of a PID. If a PID is inquired about which does not
currently belong to any CGroup, it is automatically added to the root CGroup,
in line with the behaviour on Linux.

Because only NetBSD's PUFFS (and its FUSE emulation, PERFUSE) support poll()
(but not the installation of Kernel Queues filters), while FUSE for other BSDs
doesn't, and because the `release_agent` mechanism is fundamentally fragile,
CGrpFS listens on a sequenced-packet socket in the Unix domain at
`/var/run/cgrpfs.notify`. On a process exiting, a `siginfo_t` structure is
prepared and sent as a message to every peer connected to that socket. InitWare
uses this to help track process lifecycle.

Some effort is made to be resilient to out-of-memory conditions. This is
untested and may not work. Whether libfuse is similarly resilient is another
question. There is also the problem that under OOM conditions, it is no longer
possible to update the structures in CGrpFS which describe which processes
belong to what CGroup. This might be mitigated in part by keeping some spare
memory around to use under OOM conditions, and hoping that the number of tracked
processes doesn't grow beyond its capacity while the OOM state persists.
Finally, the process filter itself can fail in-kernel under OOM conditions, and
return NOTE_TRACKERR. There is no easy way out of this without modifying the
kernel itself.

Room for Improvement
--------------------

There are several ways in which CGrpFS could be improved.

The mini-ProcFS is immutable by users and stateless, only providing information
maintained by the actual CGroups tree; it could therefore be implemented
without backing nodes to save some memory use.

Much more data than necessary is stored in each node (a full struct `stat`);
this can be reduced. And proper nodes for each pseudo-file in a CGroup directory
could be abolished too.

Much unnecessary copying goes on due to CGrpFS using the high-level libfuse
interface. Lowering to the fuse_lowlevel interface (or even directly to the
`/dev/fuse` device) could help reduce that, and hence reduce the risk of OOM
conditions causing a crash. Needless lookups also occur with the high-level
interface because it's based on path strings; the archictecture of CGrpFS more
readily fits the lower-level inode-based interface. Path lookup would also
become simpler since there would be one lookup request for each component of
the path; currently it has ugly special-cases for e.g. `mkdir`.

OOM resilience could be improved in line with the notes in the Architecture
section above.

Release agent support should be implemented for compatibility, though it's not
a reliable mechanism.

FreeBSD provides hierarchical resource control via the `rctl` system. It's not
clear whether this usefully maps to CGroups semantics, but it certainly is
worth exploring whether it could be used to provide some CGroup resource
controllers.

CGrpFS could be implemented as an in-kernel filesystem within the various BSD
kernels. CGrpFS could be hooked up more directly with the kernel's process
management, and benefit from the kernel's capacity to to deal with OOM
conditions more aggressively.

Furthering an in-kernel implementation of CGrpFS, hierarchical resource control
mechanisms could be implemented in those BSDs without them.

Contributing poll() and kevent() supprt to each BSD's FUSE/PUFFS implementation
would allow the CGroups 2.0 `cgroup.events` file to be implemented.
