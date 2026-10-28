export const meta = {
  name: 'tonight-review',
  description: 'Adversarially review landed AurOS changes; confirmed findings go to the owning agent',
  phases: [
    { title: 'Find', detail: 'one reviewer per area, reading the diff since the base' },
    { title: 'Verify', detail: 'three skeptics try to refute each finding' },
    { title: 'Route', detail: 'confirmed findings sent to the owning agent' },
  ],
}

const BASE = args.base
const REPO = '/home/user/auros-from-scratch'
const BR = 'origin/claude/brave-hypatia-x72g1m'

const COMMON = `Repository ${REPO} (read-only for you: do not edit, commit, push, or check out anything there; use \`git -C ${REPO} fetch -q origin claude/brave-hypatia-x72g1m\` then \`git -C ${REPO} diff ${BASE} ${BR} -- <paths>\` and \`git -C ${REPO} show ${BR}:<path>\` to read). Context: AurOS turns a Windows PC into a Linux desktop from one downloaded .exe, for a great-grandmother, in under 10 minutes; tonight's build must work. Read ${'`'}git -C ${REPO} show ${BR}:docs/handoff/RESUME-NOW.md${'`'} first (2 min) for the design and owner's orders (no code signing ever; Secure Boot stays on via Ubuntu's shim; never ship an unproven destructive path). Report only REAL defects with a concrete failure path from a real input/state to a wrong result: build breaks, data loss or a disk written outside its region, a grandma-blocking failure, a test that would pass while the thing it tests is broken, a security hole (e.g. passwordless root via polkit beyond intended actions), or pieces that don't fit together. No style nits. Each finding: exact file and line on the branch head, the scenario, and the smallest correct fix.`

const AREAS = [
  { key: 'forge-shell', owner: args.owners.shell,
    paths: 'build/forge build/mkimage profiles rootfs src/aurshell src/common',
    focus: 'Will tonight\'s Actions image build still succeed (bash -n is not enough: trace each new inchroot/apt line on Ubuntu 24.04 noble as it is in Oct 2026; package names exist? Mozilla apt pinning correct so Firefox replaces the snap transitional? flatpak remote-add at build time vs offline first-boot unit correct?). Polkit rules: does the auros user get ONLY install/remove of software, not arbitrary root? Does XDG_DATA_DIRS change break app discovery? Anything that makes the image fail to boot or the desktop fail to start.' },
  { key: 'staging', owner: args.owners.fastpath,
    paths: 'src/aurstage src/aurbridge build/staging build/aurbridge tools/*.sh src/aurscreen',
    focus: 'The destructive path. Any change that could write outside the intended region, skip a refusal that protected Windows, mis-handle the journal, leave Windows unbootable, or break power-cut safety. The probe change (network recorded, never required; firmware path pointed at the new root): could writing firmware_class.path or loading modules from the new root destabilise staging, and is the firmware path reset/handled if the new root is unmounted? Any test that now passes vacuously.' },
  { key: 'ci', owner: args.owners.release,
    paths: '.github/workflows build/sign build/all docs/SIGNING.md docs/RELEASE.md docs/HANDOFF-RUNBOOK.md',
    focus: 'build.yml/release.yml/tests.yml: will they actually build the image with Firefox, bake the RIGHT piece list/base URL into AurOS-Setup.exe for the release tag they create, publish asset names that the baked list and the website (releases/latest/download/AurOS-Setup.exe) expect, and refuse to publish on a failed test? Trigger conditions (subject words, paths) do what the docs say? Disk space on the runner enough? Any secret/token misuse? Can a release be published from a build whose tests failed?' },
  { key: 'website', owner: args.owners.website,
    paths: 'website .github/workflows/pages.yml tools/site',
    focus: 'Does the front page work for her: download link exact (releases/latest/download/AurOS-Setup.exe), the platform detection never blocks a real Windows x64 visitor (UA-CH missing on Firefox; Edge/Chrome report architecture "arm" for x64-on-ARM emulation etc.), steps accurate for an UNSIGNED exe in Edge and Chrome as of 2026, works without JS, relative links work under /auros-from-scratch/ on Pages, no external requests, pages.yml deploys from this non-default branch (environment protection) and what the owner must click.' },
  { key: 'e2e', owner: args.owners.e2e,
    paths: 'tools/e2e .github/workflows/e2e.yml',
    focus: 'Can this harness report success while the journey actually failed (e.g. a step judged by a screenshot that would look the same on failure, a timeout treated as pass, a cached Windows image that hides a first-install problem, the website download replaced by a direct fetch, the wizard driven by a hidden test mode instead of real input)? Does it really verify the program installed in AurOS? Anything that will make it hang for 6 hours instead of failing fast.' },
  { key: 'integration', owner: null,
    paths: '.',
    focus: 'Cross-cutting: do the pieces fit? The wizard\'s style choice values ↔ what choices.conf carries ↔ rootfs/usr/lib/auros/choices.sh applies (shell_archetype/theme names exist in shells/ and themes/)? The release asset names ↔ the installer\'s baked piece list ↔ e2e INSTALLER_URL ↔ website button? The Wi-Fi keyfile staging writes ↔ NetworkManager on the image? Flathub/polkit user name ↔ default_user in the profile? windows.yml pinned env ↔ what is published? Name the owning area of each finding in its title (fastpath, wizard, shell, website, release, e2e).' },
]

const FINDINGS = { type: 'object', properties: { findings: { type: 'array', items: { type: 'object', properties: {
  title: { type: 'string' }, severity: { type: 'string', enum: ['blocker', 'high', 'medium'] },
  file: { type: 'string' }, line: { type: 'integer' }, scenario: { type: 'string' }, fix: { type: 'string' },
  owner_area: { type: 'string', enum: ['fastpath', 'wizard', 'shell', 'website', 'release', 'e2e'] } },
  required: ['title', 'severity', 'file', 'scenario', 'fix', 'owner_area'] } } }, required: ['findings'] }
const VERDICT = { type: 'object', properties: { refuted: { type: 'boolean' }, reason: { type: 'string' } }, required: ['refuted', 'reason'] }

const ownerOf = { fastpath: args.owners.fastpath, wizard: args.owners.wizard, shell: args.owners.shell,
  website: args.owners.website, release: args.owners.release, e2e: args.owners.e2e }

const results = await pipeline(
  AREAS,
  a => agent(`${COMMON}\n\nYour area: ${a.key}. Paths: ${a.paths}. Focus: ${a.focus}\n\nIf nothing in your area changed since ${BASE}, review the current state of those paths for the same risks anyway. Return at most 8 findings, most severe first; an empty list is a fine answer.`,
    { label: `find:${a.key}`, phase: 'Find', schema: FINDINGS, effort: 'high' }),
  (r, a) => parallel(((r && r.findings) || []).map(f => () =>
    parallel([0, 1, 2].map(i => () => agent(`${COMMON}\n\nA reviewer claims this defect. Your job is to REFUTE it if you can: check the actual code on ${BR} (and the real behaviour of the tools/APIs involved). Lens ${['does the failure path actually happen with real inputs', 'is the code really as described at that file/line on the branch head (it may have changed)', 'would the proposed fix be correct and is the impact as severe as claimed'][i]}. Default to refuted=true only if you find concrete evidence it is wrong; if it holds up, refuted=false.\n\nClaim: ${JSON.stringify(f)}`,
      { label: `verify:${a.key}:${i}`, phase: 'Verify', schema: VERDICT })))
      .then(vs => ({ f, area: a.key, upheld: vs.filter(Boolean).filter(v => !v.refuted).length, reasons: vs.filter(Boolean).map(v => v.reason) }))
  )).then(list => list.filter(Boolean).filter(x => x.upheld >= 2)),
  (confirmed, a) => {
    if (!confirmed.length) return []
    const byOwner = {}
    for (const c of confirmed) { const o = c.f.owner_area; (byOwner[o] = byOwner[o] || []).push(c.f) }
    return parallel(Object.entries(byOwner).map(([o, fs]) => () => agent(
      `Send ONE message with the SendMessage tool (load it with ToolSearch "select:SendMessage") to agent id "${ownerOf[o]}". First line: "Lead review: ${fs.length} confirmed defect(s) in your area — please fix before tonight's build". Then each finding as a short numbered item: severity, file:line, the failure scenario, the fix. Add: "Verified by independent reviewers against the branch head; if you disagree, reply in your final report with evidence." Do not do anything else. Return the text you sent.\n\nFindings: ${JSON.stringify(fs)}`,
      { label: `route:${a.key}->${o}`, phase: 'Route', effort: 'low' })))
      .then(() => confirmed)
  }
)

const confirmed = results.flat().filter(Boolean)
return { confirmed: confirmed.map(c => ({ area: c.area, upheld: c.upheld, ...c.f })) }
