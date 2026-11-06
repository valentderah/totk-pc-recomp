"""Validate and emit local verb/ability selectors for host binding contexts."""

CONTEXTS = {'ULTRAHAND', 'FUSE', 'ASCEND', 'RECALL', 'AUTOBUILD', 'CAMERA', 'AMIIBO',
            'SCOPE', 'SURF', 'ROBOT', 'SLED'}


def source(config, verbs):
    if not isinstance(config, list) or not 1 <= len(config) <= 32:
        raise ValueError('Special contexts require a bounded selector list')
    names = {r['name']: i for i, r in enumerate(verbs)}
    checks, seen = [], set()
    for row in config:
        if not isinstance(row, dict) or set(row) != {'verb', 'ability', 'context'}:
            raise ValueError('Each selector requires verb, ability and context')
        name, ability, context = row['verb'], row['ability'], row['context']
        if name not in names or context not in CONTEXTS or type(ability) is not int or not -1 <= ability <= 8:
            raise ValueError('Invalid special context selector')
        key = (name, ability)
        if key in seen or (name, -1) in seen or (ability == -1 and any(v == name for v, a in seen)):
            raise ValueError('Ambiguous special context selectors')
        seen.add(key)
        condition = f'verbs[i]=={names[name]}'
        if ability >= 0:
            condition += f' && ability=={ability}'
        checks.append(f'if({condition})return IC_{context};')
    return ('int input_actions_special_context(const int32_t verbs[5],int ability){'
            'for(unsigned i=0;i<5;i++){' + ''.join(checks) + '}return -1;}\n')
