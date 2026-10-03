"""Reject generated/proprietary files and likely credentials in a public Git tree.

Checks tracked files plus nonignored additions. Findings show paths/line numbers,
never the potentially sensitive values. Upstream copyright contacts are retained.
"""
from pathlib import Path
import re
import subprocess
import sys

FORBIDDEN_ROOTS = {'assets', 'artifacts', 'build', 'cache', 'deploy', 'data', 'logs',
                   '.tools', '.public-release', 'crash logs', 'cpu-cache-fixtures'}
FORBIDDEN_EXTENSIONS = {'.exe', '.dll', '.lib', '.a', '.so', '.o', '.obj', '.exp', '.flt',
    '.vpk', '.self', '.velf', '.suprx', '.gxp', '.sfo', '.ff', '.iwd', '.iwi', '.kvap',
    '.pak', '.bik', '.mp4', '.at9', '.mp3', '.wav', '.dds', '.ico', '.png', '.jpg',
    '.jpeg', '.webp', '.zip', '.7z', '.dmp', '.bin', '.pyc'}
SECRETS = {
    'GitHub token': re.compile(r'\b(?:gh[pousr]_[A-Za-z0-9_]{20,}|github_pat_[A-Za-z0-9_]{20,})\b'),
    'AWS access key': re.compile(r'\bAKIA[0-9A-Z]{16}\b'),
    'private key': re.compile(r'-----BEGIN (?:RSA |EC |OPENSSH |DSA )?PRIVATE KEY-----'),
    'credential URL': re.compile(r'https?://[^\s/:@]+:[^\s/@]+@[^\s/]+'),
    'personal Windows home': re.compile(r'(?i)[a-z]:[/\\]+Users[/\\]+(?!Public\b|Default\b|USERNAME\b)[a-z0-9_.-]+'),
}

def audit(root):
    root = Path(root).resolve()
    result = subprocess.check_output(['git', 'ls-files', '--cached', '--others',
                                      '--exclude-standard', '-z'], cwd=root)
    names = sorted(set(n.decode('utf-8') for n in result.split(b'\0') if n))
    findings = []
    for name in names:
        relative = Path(name)
        parts = [part.lower() for part in relative.parts]
        if (parts[0] in FORBIDDEN_ROOTS or '.git' in parts or
                relative.suffix.lower() in FORBIDDEN_EXTENSIONS or
                name.lower().startswith(('kisakcod/deps/binklib/', 'kisakcod/deps/msslib/',
                                          'kisakcod/deps/steamsdk/', 'kisakcod/mileseq/')) or
                relative.name.lower() in {'.env', 'local.paths.json', 'hosts.yml'}):
            findings.append((name, 0, 'excluded payload/path'))
            continue
        path = root / relative
        if not path.is_file(): continue
        if path.is_symlink() or not path.resolve().is_relative_to(root):
            findings.append((name, 0, 'link outside the public tree'))
            continue
        data = path.read_bytes()
        try:
            content = data.decode('utf-8-sig')
            if '\0' in content: raise UnicodeError('binary content')
        except UnicodeError:
            findings.append((name, 0, 'non-text payload'))
            continue
        for number, line in enumerate(content.splitlines(), 1):
            for kind, pattern in SECRETS.items():
                if pattern.search(line): findings.append((name, number, kind))
    return names, findings

if __name__ == '__main__':
    root = Path(__file__).resolve().parents[1]
    names, findings = audit(root)
    for name, line, kind in findings:
        print(f'{name}:{line}: {kind}')
    print(f'Public source audit: {len(names)} files, {len(findings)} findings')
    raise SystemExit(1 if findings else 0)
