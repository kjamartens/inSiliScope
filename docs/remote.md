# Run inSiliScope on a remote Linux server

You can have a more powerful Linux machine do the computing, such as an analysis server, a group workstation or an
HPC cluster, while you keep working in the browser viewer as usual. The viewer sends the settings over SSH, and the
server sends the frames back: live, Records, movies and ground truth all run there.

- **What you need:** a login to the server and the `ssh` command on your PC. Windows 10/11, macOS and Linux all
  include `ssh`.
- **What you don't need:** admin rights on either machine, or installing anything yourself. The server fetches the
  inSiliScope version that matches your viewer by itself the first time you connect.

Micro-Manager support is coming; for now this page is about the viewer.

## What you need

| | |
|---|---|
| **Server** | Linux, x86-64, glibc 2.28 or newer: any distribution from about 2019 on (Ubuntu 20.04+, RHEL/Rocky/Alma 8+, Debian 11+). About 200 MB free in your home directory. |
| **Login** | An account you can reach with `ssh`, directly or through a gateway (also called a jump or bastion host). |
| **Internet on the server** | Recommended: the server downloads inSiliScope from GitHub. Without it, see [Servers without internet](#servers-without-internet). |
| **Your PC** | A terminal with `ssh`: PowerShell on Windows, Terminal on macOS and Linux. For the viewer: Chrome, Edge or Firefox. |

Remote computing helps most for Records of large fields (1000 px and up), ground truth, structured illumination and
BFP light maps, and long multi-D plans. For a small live field your own PC is often just as fast: every frame makes
a round trip over the network.

## Step 1: check that you can log in

Open PowerShell (Windows) or a terminal (macOS, Linux) and run:

```powershell
ssh jdoe@compute.example.org hostname
```

If the server is behind a gateway:

```powershell
ssh -J jdoe@gateway.example.org jdoe@compute.example.org hostname
```

Answer `yes` the first time ssh asks whether to trust the server. The server prints its name.

## Step 2: log in without a password (recommended)

With a key you don't type a password each time you connect.

1. Make a key (press Enter at each question):
   ```powershell
   ssh-keygen -t ed25519
   ```
2. Copy it to the gateway (skip this without a gateway):
   ```powershell
   type $env:USERPROFILE\.ssh\id_ed25519.pub | ssh jdoe@gateway.example.org "mkdir -p ~/.ssh && cat >> ~/.ssh/authorized_keys"
   ```
3. Copy it to the server:
   ```powershell
   type $env:USERPROFILE\.ssh\id_ed25519.pub | ssh -J jdoe@gateway.example.org jdoe@compute.example.org "mkdir -p ~/.ssh && cat >> ~/.ssh/authorized_keys"
   ```
   On macOS and Linux, `ssh-copy-id` does the same.
4. Run Step 1's command again: it no longer asks for a password.

Some institutes don't allow keys on their gateway or ask for a second factor; then ask your IT department how to set
up key-based SSH.

## Step 3: give the server a short name (recommended)

Add these lines to your SSH configuration file (Windows: `%USERPROFILE%\.ssh\config`; macOS and Linux:
`~/.ssh/config`):

```text
Host isc-server
    HostName compute.example.org
    User jdoe
    ProxyJump jdoe@gateway.example.org    # leave out without a gateway
    ServerAliveInterval 30
```

From now on `ssh isc-server` is enough, and the viewer only needs the name **isc-server**.

## Step 4: check the server (once)

```powershell
ssh isc-server "curl -fsSL https://kjamartens.github.io/inSiliScope/remote.sh -o ~/.isc-remote.sh && sh ~/.isc-remote.sh check"
```

It writes only the small launcher script `~/.isc-remote.sh` and prints what the server has, for example:

```text
inSiliScope remote check
  system      Ubuntu 24.04.1 LTS, glibc 2.39, x86_64
  CPUs        96 (load 3.10)
  memory      1510 GB (1203 GB free)
  disk        /home/jdoe/.local/share/insiliscope: 211G free
  internet    github.com reachable
  scheduler   none found (runs directly on this machine)
  installed
This machine can run inSiliScope.
```

## Use it from the viewer

1. In the viewer, open ☰ › **Remote server…**
2. Fill in the box:
    - **Server:** `isc-server`, the name from Step 3 (or `jdoe@compute.example.org`).
    - **Send to the server:** *Everything, live included* (the default) or *Records, movies and ground truth* (live
      stays in the browser).
    - **Threads per engine:** how many CPUs each of the viewer's engines may use there (the live view and the
      movies are one engine each). Empty: half the server's CPUs. On a shared machine, leave room for others.
3. Click **Use it**, then **Copy the command**, paste it into PowerShell and leave that window open. It looks like
   this:
   ```powershell
   ssh -o ExitOnForwardFailure=yes -o ServerAliveInterval=30 -L 18765:/tmp/isc-3f9a0c1e/s isc-server "(curl -fsSL https://kjamartens.github.io/inSiliScope/remote.sh -o ~/.isc-remote.sh || wget -qO ~/.isc-remote.sh https://kjamartens.github.io/inSiliScope/remote.sh) && sh ~/.isc-remote.sh serve --version v0.2.0 --socket /tmp/isc-3f9a0c1e/s --token 5c0e…"
   ```
   The first time, the server downloads its inSiliScope (once per version), then prints:
   ```text
   inSiliScope server 0.2.0 on compute (96 CPUs, using 48 per engine)
   ready -- keep this window open; Ctrl+C stops the server
   ```
4. The viewer connects by itself within a few seconds. Chrome and Edge may ask whether the site may access devices
   on your local network: choose **Allow** (the "device" is the end of the SSH tunnel on your own PC).
5. The menu now says **Computing on compute**, and the live view's info line ends with the server's name. The
   Record's frames stream into the viewer as usual and *Save* writes them on your PC.

**To stop:** close the PowerShell window or press Ctrl+C. The server ends with it, and the viewer goes on computing in
the browser; a live view keeps running from where it was (the sample's light so far is replayed in the browser).
**Stop using it** in the box does the same from the viewer's side. Without a connection for 30 minutes the server
also stops by itself.

The command is the same every time on this computer (its socket name and token are kept in the browser), so you can
keep it in a script or a terminal profile. **Run it again** after it stopped and the viewer reconnects by itself.

## Clusters with a job scheduler (Slurm)

On a cluster the login node is for logging in, not for computing. Let inSiliScope run in a job: in the box's
**More**, fill in **Run through** with your scheduler's command, for example

```text
srun --cpus-per-task=32 --mem=64G --time=02:00:00 --partition=compute
```

- The server then starts inside a job on a compute node; a small relay on the login node connects the tunnel to it.
- The viewer waits until the job starts, which on a busy cluster can take a while (the terminal says so every 30 s).
- The job's time limit also ends your session. The job ends when you close the terminal.

## Updates

- **Updates happen by themselves.** The viewer's command asks for exactly the viewer's own version; the server
  downloads it the first time it is asked (once, ~40 MB) and keeps the three versions used last.
- **Why the same version on both ends:** then the frames made on the server are the same, byte for byte, as the ones
  your browser makes. A server of another version is not used (the box says why).

| | |
|---|---|
| Where it lives | `~/.local/share/insiliscope/` (versions) and `~/.cache/insiliscope/` (cell caches) |
| Remove it | `ssh isc-server "sh ~/.isc-remote.sh uninstall; rm -rf ~/.cache/insiliscope ~/.isc-remote.sh"` |

## Servers without internet

1. On your PC, download `insiliscope-server-linux-x86_64.tar.gz` of your viewer's version from the
   [release page](https://github.com/kjamartens/inSiliScope/releases), and `remote.sh` from
   `https://kjamartens.github.io/inSiliScope/remote.sh`.
2. Copy both to the server:
   ```powershell
   scp insiliscope-server-linux-x86_64.tar.gz remote.sh isc-server:
   ```
3. Install it (replace v0.2.0 by your version):
   ```powershell
   ssh isc-server "mv remote.sh ~/.isc-remote.sh && sh ~/.isc-remote.sh install --version v0.2.0 --from insiliscope-server-linux-x86_64.tar.gz"
   ```
4. In the command the viewer gives, leave out the download part: start it at `sh ~/.isc-remote.sh serve`.

## Render a recording without the viewer

Save a recording's settings from the viewer (☰ › Save), copy the file to the server, and render it there:

```powershell
scp run.isc.json isc-server:
ssh isc-server "sh ~/.isc-remote.sh render --version v0.2.0 < run.isc.json > run.tif"
scp isc-server:run.tif .
```

## Security and privacy

- **Nothing passes through anyone else:** the traffic goes through your own SSH connection. There is no inSiliScope
  service in between.
- **Nothing open to others:** the server opens no network port. It listens on a socket in a folder in `/tmp` that
  only your account can open, and it asks for the token in the command. Under **Run through** the job's server
  listens on the cluster's internal network for the relay, with the same token; that hop is not encrypted.
- **Only what the computing needs is sent:** settings, the live sample's light so far, map images you added, and the
  frames back.
- **Easy on shared machines:** the server is an ordinary process of your account, at low priority (`nice 10`), with
  the threads you allowed.
- **For your IT department:** *a user-space process started over SSH; no root, no open ports, no system changes; it
  stops when the SSH session ends.*

## Troubleshooting

| You see | What to do |
|---|---|
| `Permission denied (publickey)` | The key isn't on the server or the gateway: redo Step 2. |
| `glibc 2.17 found, 2.28 needed` | The server's system is too old for the server program. |
| `bind: address already in use` (port 18765) | Another connection is still open: close it, or set another port in the box's **More**. |
| The box says *Waiting for the server* | Is the terminal still open, and does it say `ready`? In Chrome, check the site's local-network permission (the padlock › Site settings). |
| *Not used: the server is another version* | The command in the box asks for your viewer's version: copy it again (an old command asks for an old version). |
| The connection dropped | The viewer went on in the browser. Run the command again; the viewer reconnects by itself. |
| `cannot download` | The server has no internet: see [Servers without internet](#servers-without-internet). |

### Example: TU Delft ImPhys servers

```text
Host isc-server
    HostName jupiter-imphys.tnw.tudelft.nl
    User <netid>
    ProxyJump <netid>@linux-bastion-ex.tudelft.nl
```

These servers have no scheduler: leave *Run through* empty, and keep *Threads per engine* well below the CPU count on
these shared machines.
