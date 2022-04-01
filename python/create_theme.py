import json


def create_pidgin_theme_file():
    begin_text = [
        "Name=Slack",
        "Description=Slack Emojis ported to pidgin",
        "Icon=1F40C.png",
        "Author=Slack",
        "",
        "[default]"
    ]
    emoji_lines = []
    data = json.load(open('emoji_pretty.json', 'r'))
    for emoji in data:
        unicode_point = emoji['unified']
        # If the point contains a - we split it

        current_emojis = []
        for sub in unicode_point.split('-'):
            current_emojis.append(chr(int(sub, base=16)));
            print(sub)

        emoji_lines.append(f'{unicode_point}.png\t{"".join(current_emojis)}')
        #emoji_lines.append(f'{emoji["unified"]}.png\t🙅🏿‍♂️')


    for size in ['medium', 'large']:
        with open(f'{size}/theme', 'w') as f:
            f.write('\n'.join(begin_text))
            f.write('\n')
            f.write('\n'.join(emoji_lines))

def create_theme():
    create_pidgin_theme_file()
if __name__ == "__main__":
    create_theme()