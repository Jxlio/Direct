import test from 'node:test';
import assert from 'node:assert/strict';
import worker, {Directory,subscriptions} from './worker.mjs';
const a='a'.repeat(64),b='b'.repeat(64);
test('bounded subscriptions reject duplicates and malformed IDs',()=>{
  assert.deepEqual(subscriptions(`JOIN|${a}0`),[{id:a,role:0}]);
  for(const text of ['',`JOIN|${a}0|${a}1`,`JOIN|${a}2`,'JOIN|'+('x'.repeat(1200))]) assert.equal(subscriptions(text),null);
});
test('HTTP access requires the private group capability before Durable Object use',async()=>{
  let called=0;
  const env={GROUP_ACCESS:a,DIRECTORY:{idFromName:()=>'',get:()=>({fetch:()=>{called++;return new Response('ok');}})}};
  assert.equal((await worker.fetch(new Request('https://example.com/v1/connect'),env)).status,401);
  assert.equal((await worker.fetch(new Request('https://example.com/v1/connect',{headers:{Authorization:`Bearer ${a}`}}),env)).status,426);
  assert.equal(called,0);
});
test('opaque frames are forwarded only to the opposite subscribed peer',()=>{
  const socket=(rooms)=>({readyState:1,state:{rooms,tokens:16,at:Date.now()},sent:[],deserializeAttachment(){return this.state;},serializeAttachment(s){this.state=s;},send(x){this.sent.push(x);},close(){this.readyState=3;}});
  const source=socket([{id:a,role:0}]),target=socket([{id:a,role:1}]),outsider=socket([{id:b,role:1}]);
  const directory=new Directory({getWebSockets:()=>[source,target,outsider]});
  const frame=new Uint8Array(120);frame.fill(0xaa,0,32);frame[32]=1;
  directory.webSocketMessage(source,frame.buffer);
  assert.equal(target.sent.length,1);assert.equal(outsider.sent.length,0);
  directory.webSocketClose(source);assert.equal(target.sent[1][32],2);
});
test('a replaced socket cannot mark its replacement offline',()=>{
  const make=(role)=>({readyState:1,deserializeAttachment:()=>({rooms:[{id:a,role}]}),sent:[],send(x){this.sent.push(x);}});
  const old=make(0),replacement=make(0),peer=make(1);old.readyState=3;
  const directory=new Directory({getWebSockets:()=>[old,replacement,peer]});
  directory.offline(old,[{id:a,role:0}]);assert.equal(peer.sent.length,0);
});
