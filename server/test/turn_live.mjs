// Asks a running service for relay credentials, the way an app in a voice channel does.
//   node test/turn_live.mjs https://murmur-service.<account>.workers.dev
// Prints whether a relay came back (never the credentials). Exit code 0 only if one did.
import { Identity, RemoteHub, newGuildId } from './harness.mjs';

const base = process.argv[2] || process.env.MURMUR_SERVICE_URL;
if (!base) { console.error('Usage: node test/turn_live.mjs <service address>'); process.exit(2); }
const hub = new RemoteHub(base);
try {
  const guild = newGuildId(), owner = await Identity.create(), client = await hub.connect(guild);
  await client.login(owner, guild);
  const created = await client.ask({ t: 'guild.create', ...(await owner.encFields(guild)), title: 'Essai relais' });
  if (!created.member) throw new Error('could not open a test server');
  const answer = await client.ask({ t: 'turn.get' });
  const servers = Array.isArray(answer.iceServers) ? answer.iceServers : [];
  const urls = servers.flatMap((s) => (Array.isArray(s.urls) ? s.urls : [s.urls]));
  const relays = urls.filter((u) => /^turns?:/.test(u));
  console.log(relays.length
    ? `Relay available: ${relays.length} turn address(es), ${servers.some((s) => s.username && s.credential) ? 'with temporary credentials' : 'WITHOUT credentials'}.`
    : 'No relay: the service has no TURN secrets (or Cloudflare refused them).');
  process.exitCode = relays.length ? 0 : 1;
} catch (error) {
  console.error('ERROR', error.message);
  process.exitCode = 2;
} finally { hub.close(); }
