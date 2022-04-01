import json
import urllib.request
import os



def run():
    with open('emoji_pretty.json', 'r') as f:
        data = json.load(f)
        for emoji in data:
            for size in ('medium', 'large'):
                # Check if the image exists already

                unicode_point = emoji['unified']
                if not os.path.isfile(f'{size}/{unicode_point}.png'):
                    url = f"https://a.slack-edge.com/production-standard-emoji-assets/13.0/google-{size}/{unicode_point.lower()}.png"
                    print(url + '\n')
                    try:
                        urllib.request.urlretrieve(url, f'{size}/{unicode_point}.png')
                    except:
                        print(f'Failed to download {url}\n')


if __name__ == "__main__":
    run()
