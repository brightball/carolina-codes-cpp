-- Minimal v1_* views for the read-only API. Columns match the queries in src/main.cpp.
DROP VIEW IF EXISTS v1_years;
DROP VIEW IF EXISTS v1_speakers;
DROP VIEW IF EXISTS v1_talks;
DROP VIEW IF EXISTS v1_sponsors;
DROP VIEW IF EXISTS v1_year_sponsors;
DROP VIEW IF EXISTS v1_sponsorships;

CREATE VIEW v1_years AS
SELECT *
FROM (
  VALUES
    (2026, '2026'::text, 'Carolina Code Conference 2026'::text, 'published'::text),
    (2025, '2025'::text, 'Carolina Code Conference 2025'::text, 'archived'::text)
) AS t(year, slug, name, status);

CREATE VIEW v1_speakers AS
SELECT *
FROM (
  VALUES
    (
      'ada-lovelace'::text, 'Ada'::text, 'Lovelace'::text, 'Ada Lovelace'::text,
      'Analyst'::text, 'Notes on the engine'::text, 'Analytical Engines'::text,
      'London'::text, '/photos/ada.jpg'::text, 'https://twitter.com/ada'::text,
      'https://linkedin.com/in/ada'::text, 'https://ada.example'::text,
      'https://github.com/ada'::text, true
    ),
    (
      'grace-hopper'::text, 'Grace'::text, 'Hopper'::text, 'Grace Hopper'::text,
      'Rear admiral'::text, 'Compilers'::text, 'US Navy'::text, 'New York'::text,
      '/photos/grace.jpg'::text, 'https://twitter.com/grace'::text,
      'https://linkedin.com/in/grace'::text, 'https://grace.example'::text,
      'https://github.com/grace'::text, true
    ),
    (
      'barbara-liskov'::text, 'Barbara'::text, 'Liskov'::text, 'Barbara Liskov'::text,
      'Professor'::text, 'Abstraction'::text, 'MIT'::text, 'Boston'::text,
      '/photos/barbara.jpg'::text, 'https://twitter.com/barbara'::text,
      'https://linkedin.com/in/barbara'::text, 'https://barbara.example'::text,
      'https://github.com/barbara'::text, false
    )
) AS t(
  slug, first_name, last_name, name, tagline, bio, company, location, photo_path,
  twitter_url, linkedin_url, website_url, github_url, featured
);

CREATE VIEW v1_talks AS
SELECT *
FROM (
  VALUES
    (
      'analytical-engine'::text, 'The Analytical Engine'::text, 'A talk'::text,
      'talk'::text, 'yt-ada'::text, 2026, 'ada-lovelace'::text,
      ARRAY['English']::text[], ARRAY['history']::text[]
    ),
    (
      'notes-1843'::text, 'Notes'::text, 'Older talk'::text, 'talk'::text,
      'yt-ada-2'::text, 2025, 'ada-lovelace'::text, ARRAY['English']::text[],
      ARRAY['math']::text[]
    ),
    (
      'compilers'::text, 'Compilers'::text, 'COBOL and friends'::text, 'talk'::text,
      'yt-grace'::text, 2026, 'grace-hopper'::text, ARRAY['English']::text[],
      ARRAY['compilers']::text[]
    ),
    (
      'abstraction'::text, 'Data Abstraction'::text, 'Types'::text, 'talk'::text,
      'yt-barbara'::text, 2026, 'barbara-liskov'::text, ARRAY['English']::text[],
      ARRAY['types']::text[]
    )
) AS t(
  slug, title, description, format, youtube_id, year, speaker_slug, languages, topics
);

CREATE VIEW v1_sponsors AS
SELECT *
FROM (
  VALUES
    (
      'acme'::text, 'Acme'::text, 'https://acme.example'::text, '/logos/acme.png'::text,
      'Conference sponsor'::text, 'https://twitter.com/acme'::text,
      'https://linkedin.com/company/acme'::text, 'https://youtube.com/acme'::text,
      'https://instagram.com/acme'::text, 'https://facebook.com/acme'::text
    )
) AS t(
  slug, name, website, logo_path, description, twitter_url, linkedin_url, youtube_url,
  instagram_url, facebook_url
);

CREATE VIEW v1_year_sponsors AS
SELECT *
FROM (
  VALUES
    (
      'acme'::text, 'Acme'::text, 'https://acme.example'::text, '/logos/acme.png'::text,
      'Conference sponsor'::text, 'Gold blurb'::text, 'gold'::text, true, 2026,
      'https://twitter.com/acme'::text, 'https://linkedin.com/company/acme'::text,
      'https://youtube.com/acme'::text, 'https://instagram.com/acme'::text,
      'https://facebook.com/acme'::text
    )
) AS t(
  slug, name, website, logo_path, description, blurb, tier, featured, year, twitter_url,
  linkedin_url, youtube_url, instagram_url, facebook_url
);

CREATE VIEW v1_sponsorships AS
SELECT *
FROM (
  VALUES
    ('acme'::text, 2026, 'gold'::text, 'Gold blurb'::text, true)
) AS t(sponsor_slug, year, tier, blurb, featured);
