"""Public release boundaries use synthetic files, never retail test fixtures."""
import importlib.util
from pathlib import Path
import subprocess
import pytest

ROOT = Path(__file__).resolve().parents[4]

def module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'tools' / (name + '.py'))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result

def fake_install(root):
    for name in ('iw3mp.exe', 'localization.txt', 'main/synthetic.iwd',
                 'zone/english/mp_shipment.ff'):
        target = root / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(b'synthetic test input')
    return root

def test_preflight_leaves_owned_data_unchanged(tmp_path, monkeypatch):
    builder = module('prepare_owned_content')
    monkeypatch.setattr(builder, 'WORKSPACE', tmp_path / 'project')
    owned = fake_install(tmp_path / 'owned')
    before = {p.relative_to(owned): p.read_bytes() for p in owned.rglob('*') if p.is_file()}
    assert builder.check_install(owned, ['mp_shipment']) == owned.resolve()
    assert {p.relative_to(owned): p.read_bytes() for p in owned.rglob('*') if p.is_file()} == before

def test_preflight_rejects_overlapping_install_and_missing_inputs(tmp_path, monkeypatch):
    builder = module('prepare_owned_content')
    project = tmp_path / 'project'
    monkeypatch.setattr(builder, 'WORKSPACE', project)
    with pytest.raises(ValueError, match='separate directories'):
        builder.check_install(fake_install(project / 'owned'), ['mp_shipment'])
    owned = fake_install(tmp_path / 'owned')
    with pytest.raises(ValueError, match='Missing Steam COD4 input'):
        builder.check_install(owned, ['mp_shipment', 'mp_crash'])

def test_audit_rejects_payloads_credentials_and_personal_paths(tmp_path):
    scanner = module('audit_public_source')
    subprocess.run(['git', 'init', '-q', str(tmp_path)], check=True)
    (tmp_path / 'synthetic.kvap').write_bytes(b'synthetic')
    token = 'ghp_' + 'x' * 30
    private_path = 'C:' + '/Users/' + 'private/example'
    (tmp_path / 'example.txt').write_text(token + '\n' + private_path, encoding='utf-8')
    names, findings = scanner.audit(tmp_path)
    assert len(names) == 2
    assert {kind for _, _, kind in findings} == {
        'excluded payload/path', 'GitHub token', 'personal Windows home'}
    assert all(token not in str(finding) and private_path not in str(finding) for finding in findings)
