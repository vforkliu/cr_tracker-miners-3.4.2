# Copyright (C) 2011, Nokia (ivan.frade@nokia.com)
# Copyright (C) 2019-2020, Sam Thursfield (sam@afuera.me.uk)
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the
# Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
# Boston, MA  02110-1301, USA.
#

import logging
import os
import sys
import time
import unittest as ut

import fixtures


log = logging.getLogger(__name__)


class WritebackKeepDateTest (fixtures.TrackerWritebackTest):

    def setUp(self):
        super(WritebackKeepDateTest, self).setUp()
        self.favorite = self.__prepare_favorite_tag()

    def __prepare_favorite_tag(self):
        # Check here if favorite has tag... to make sure writeback is actually writing
        results = self.tracker.query("""
             SELECT ?label WHERE { nao:predefined-tag-favorite nao:prefLabel ?label }""")

        if len(results) == 0:
            self.tracker.update("""
             INSERT { nao:predefined-tag-favorite nao:prefLabel 'favorite'}
             WHERE { nao:predefined-tag-favorite a nao:Tag }
             """)
            return "favorite"
        else:
            return str(results[0][0])

    def test_01_NB217627_content_created_date(self):
        """
        NB#217627 - Order if results is different when an image is marked as favorite.
        """
        jpeg_path = self.prepare_test_image(self.datadir_path('writeback-test-1.jpeg'))
        tif_path = self.prepare_test_image(self.datadir_path('writeback-test-2.tif'))
        png_path = self.prepare_test_image(self.datadir_path('writeback-test-4.png'))

        query_images = """
          SELECT nie:url(?u) ?contentCreated WHERE {
              ?u a nfo:Visual ;
              nfo:fileLastModified ?contentCreated
          } ORDER BY ?contentCreated
          """
        results = self.tracker.query(query_images)
        self.assertEqual(len(results), 3, results)

        log.debug("Waiting 2 seconds to ensure there is a noticiable difference in the timestamp")
        time.sleep(2)

        initial_mtime = jpeg_path.stat().st_mtime

        # This triggers the writeback
        mark_as_favorite = """
         INSERT {
           ?u a rdfs:Resource ; nao:hasTag nao:predefined-tag-favorite .
         } WHERE {
           ?u nie:url <%s> .
         }
        """ % jpeg_path.as_uri()
        self.tracker.update(mark_as_favorite)
        log.debug("Setting favorite in <%s>", jpeg_path.as_uri())

        self.wait_for_file_change(jpeg_path, initial_mtime)

        # Check the value is written in the file
        metadata = fixtures.get_tracker_extract_output(self.extra_env, jpeg_path, output_format='json-ld')

        tags = metadata.get('nao:hasTag', [])
        tag_names = [tag['nao:prefLabel'] for tag in tags]
        self.assertIn(self.favorite, tag_names,
                      "Tag hasn't been written in the file")

        # Now check the modification date of the files and it should be the same :)
        new_results = self.tracker.query(query_images)
        # for (uri, date) in new_results:
        ##     print "Checking dates of <%s>" % uri
        ##     previous_date = convenience_dict[uri]
        ##     print "Before: %s \nAfter : %s" % (previous_date, date)
        ##     self.assertEquals (date, previous_date, "File <%s> has change its contentCreated date!" % uri)

        # Indeed the order of the results should be the same
        for i in range(0, len(results)):
            self.assertEqual(results[i][0], new_results[i][0], "Order of the files is different")
            self.assertEqual(results[i][1], new_results[i][1], "Date has change in file <%s>" % results[i][0])


if __name__ == "__main__":
    fixtures.tracker_test_main()
