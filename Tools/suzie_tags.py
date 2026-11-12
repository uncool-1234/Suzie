"""Generic gameplay tag generator: reads Default__GameplayTagsSettings from a jmap and writes
<project>/Config/Tags/<name>.ini (plain repeated GameplayTagList= keys, NO '+' prefix: FConfigFile yields no array entries for '+' lines
in Config/Tags files) and <project>/Config/DefaultGameplayTags.ini (ImportTagsFromConfig, redirects).
usage: suzie_tags.py --jmap <file.jmap> --project <project dir> [--tags-ini Config/Tags/GameTags.ini]   (see Profiles/<G>/tags/tags.json)"""
import argparse, json, os
ap = argparse.ArgumentParser()
ap.add_argument('--jmap', required=True)
ap.add_argument('--project', required=True)
ap.add_argument('--tags-ini', default='Config/Tags/GameTags.ini')
a = ap.parse_args()
o = json.load(open(a.jmap, encoding='utf8'))['objects']
pv = o[[n for n in o if n.endswith('Default__GameplayTagsSettings')][0]]['property_values']
tags_ini = os.path.join(a.project, a.tags_ini)
cfg_ini = os.path.join(a.project, 'Config', 'DefaultGameplayTags.ini')
os.makedirs(os.path.dirname(tags_ini), exist_ok=True)
NL = '\n'
with open(tags_ini, 'w', encoding='utf8') as f:
    f.write('[/Script/GameplayTags.GameplayTagsList]' + NL)
    for t in pv['GameplayTagList']:
        f.write('GameplayTagList=(Tag="%s",DevComment="%s")' % (t['Tag'], t.get('DevComment', '').replace('"', "'")) + NL)
with open(cfg_ini, 'w', encoding='utf8') as f:
    f.write('[/Script/GameplayTags.GameplayTagsSettings]' + NL + 'ImportTagsFromConfig=True' + NL + 'WarnOnInvalidTags=True' + NL)
    for r in pv['GameplayTagRedirects']:
        f.write('+GameplayTagRedirects=(OldTagName="%s",NewTagName="%s")' % (r['OldTagName'], r['NewTagName']) + NL)
print(len(pv['GameplayTagList']), 'tags,', len(pv['GameplayTagRedirects']), 'redirects')
