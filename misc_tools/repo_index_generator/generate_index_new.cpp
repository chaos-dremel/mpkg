#include <mpkgsupport/mpkgsupport.h>
#include <cstdio>
#include <string>
#include <vector>


#include <my_global.h>
#include <mysql.h>
using namespace std;

int index_counter = 0;

string getDepConditionBack(int db_in) {
	switch(db_in) {
		case 1: return "more";
		case 2: return "less";
		case 3: return "equal";
		case 4: return "notequal";
		case 5: return "atleast";
		case 6: return "notmore";
		case 7: return "any";
	}
	return "";
}
void report_failure(int line, const char *error = NULL) {
	fprintf(stderr, "EPIC FAIL at line %d\n", line);
	if (error) fprintf(stderr, "%s\n", error);
	abort();
}

void log_query(string query, string out_file = "big_query.log") {
	FILE *f = fopen(out_file.c_str(), "a");
	if (f) {
		fprintf(f, string(query + "\n").c_str());
		fclose(f);
	}

}
string getFancyDistro(const char *drepo, const char *darch, const char * dbranch) {
	string repo, arch, branch;
	if (drepo) repo = drepo;
	if (darch) arch = darch;
	if (dbranch) branch = dbranch;
	return repo + "-" + branch + "-" + arch;
}

string getLocation(vector<string> drepo, vector<string> darch, vector<string> dbranch, const char *arch, const char *distro, const char *repo, string relative) {

	string ret;
	// Note: ORDER MATTERS HERE! Otherwise, return value will be incorrect.
	if (darch.size()==1) ret = "";
	else if (dbranch.size()==1) ret = "/" + string(arch) + "/repository";
	else if (drepo.size()==1) ret = "/" + string(distro) + "/" + string(arch) + "/repository";
	else ret = "/" + string(repo) + "/" + string(distro) + "/" + string(arch) + "/repository";

	if (relative.size()>1 && relative[0]=='.' && relative[1]=='/') relative = relative.substr(2);

	return ret + "/" + relative;
}

string getIndexPath( vector<string> drepo, vector<string> darch, vector<string> dbranch) {
	string index_path = "package_tree"; // Repository root, you may specify another one
	if (drepo.size()==1) index_path += "/" + drepo[0];
	if (dbranch.size()==1) index_path += "/" + dbranch[0];
	if (darch.size()==1) index_path += "/" + darch[0] + "/repository";
	return index_path;
}
void generateIndex2(MYSQL &conn, vector<string> drepo, vector<string> darch, vector<string> dbranch, string index_path = "", bool clear_branch = false) {
	index_counter++;
	string fancydistro;
	string search_subquery;

	if (index_path.empty()) index_path = getIndexPath(drepo, darch, dbranch);
	printf("IDXPATH: %s\n", index_path.c_str());
#ifdef DEBUG
	return;
#endif


	if (dbranch.size()>0) {
		search_subquery += " AND locations.distro_version IN (";
		for (size_t i=0; i<dbranch.size(); ++i) {
			search_subquery += "'" + dbranch[i] + "'";
			//search_subquery += "'" + dbranch[i] + "','" + dbranch[i] + "_deprecated'";
			if (i<dbranch.size()-1) search_subquery += ",";
			else search_subquery += ")";
		}
	}
	else {
		search_subquery += " AND locations.distro_version='8.0' ";
	}

	if (drepo.size()>0) {
		search_subquery += " AND locations.server_url IN (";
		for (size_t i=0; i<drepo.size(); ++i) {
			search_subquery += "'" + drepo[i] + "'";
			if (i<drepo.size()-1) search_subquery += ",";
			else search_subquery += ")";
		}
	}
	else {
		// Blacklist badversions repo from global index
		search_subquery += " AND locations.server_url!='badversions' ";
	}




	if (darch.size()>0) {
		search_subquery += " AND locations.distro_arch IN (";
		for (size_t i=0; i<darch.size(); ++i) {
			search_subquery += "'" + darch[i] + "'";
			if (i<darch.size()-1) search_subquery += ",";
			else search_subquery += ")";
		}
	}

	if (clear_branch) dbranch.clear();	
	printf("Generating index for %s\n", index_path.c_str());
	MYSQL_RES *packages;
	int res;
	string query = string("SELECT DISTINCT packages.package_id, \
			packages.package_name, \
			packages.package_version, \
			packages.package_arch, \
			packages.package_build, \
			packages.package_compressed_size, \
			packages.package_installed_size, \
			packages.package_short_description, \
			packages.package_description, \
			packages.package_changelog, \
			packages.package_packager, \
			packages.package_packager_email, \
			packages.package_md5, \
			packages.package_filename, \
			packages.package_repository_tags, \
			packages.package_provides, \
			packages.package_conflicts, \
			locations.location_path, \
			locations.distro_arch, \
			locations.distro_version, \
			locations.server_url \
			FROM packages, locations WHERE packages.package_id=locations.packages_package_id " + search_subquery);
	res = mysql_query(&conn, query.c_str());

	if (res) report_failure(__LINE__);

	//printf("Count: %d\n", mysql_field_count(&conn));
	packages = mysql_store_result(&conn);

	//printf("Checking out ABUILD list...\n");
	MYSQL_RES *abuilds;
	query = "SELECT package_id, filename FROM abuilds";
	res = mysql_query(&conn, query.c_str());
	if (res) report_failure(__LINE__);

	abuilds = mysql_store_result(&conn);
	printf("Found: %d packages, starting to generate index in %s\n", mysql_num_rows(packages), index_path.c_str());

	FILE *xml = fopen(string(index_path + "/packages.xml").c_str(), "w");
	if (!xml) {
		perror("Failed to open packages.xml file for writing");
		abort();
	}
	FILE *package_list = fopen(string(index_path + "/package_list").c_str(), "w");
	if (!package_list) {
		perror("Failed to open package_list file for writing");
		abort();
	}

	fprintf(xml, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<repository>\n");
	fprintf(package_list, string("# Package list for " + index_path + ", generated by server\n").c_str());

	// Add package description URL's to XML tree
	vector<string> langs = ReadFileStrings("wiki_language.list");

	langs.push_back("all");
	fprintf(xml, "<descriptions>\n");
	for (size_t i=0; i<langs.size(); ++i) {
		if (langs[i].empty()) continue;
		if (langs[i]!="all") fprintf(xml, "\t<description lang=\"%s\" path=\"rsync://packages.agilialinux.net/descriptions/%s\" />\n", langs[i].c_str(), langs[i].c_str()); // FIXME: do not hardcode paths
		else fprintf(xml, "\t<description lang=\"all\" path=\"rsync://packages.agilialinux.net/descriptions\" />\n");
		fprintf(xml, "\t<description lang=\"%s\" path=\"http://packages.agilialinux.net/descriptions/%s.tar.xz\" />\n", langs[i].c_str(), langs[i].c_str()); // FIXME: do not hardcode paths here too
	}

	fprintf(xml, "</descriptions>\n\n");

	// Generating XML...
	MYSQL_ROW row, drow, abuild, trow, crow, coptrow;
	MYSQL_RES *deps, *tags, *confs, *coptres;

	string attr;

	while (row = mysql_fetch_row(packages)) {
		fancydistro = getFancyDistro(row[20], row[18], row[19]);
		fprintf(package_list, row[1]);
		fprintf(xml, "<package>\n");
		fprintf(xml, "\t<name>%s</name>\n", row[1]);
		fprintf(xml, "\t<version>%s</version>\n", row[2]);
		fprintf(xml, "\t<arch>%s</arch>\n", row[3]);
		fprintf(xml, "\t<build>%s</build>\n", row[4]);

		if (row[15] && cutSpaces(row[15])!="") fprintf(xml, "\t<provides>%s</provides>\n", cutSpaces(row[15]).c_str());
		if (row[16] && cutSpaces(row[16])!="") fprintf(xml, "\t<conflicts>%s</conflicts>\n", cutSpaces(row[16]).c_str());

		fprintf(xml, "\t<compressed_size>%s</compressed_size>\n", row[5]);
		fprintf(xml, "\t<installed_size>%s</installed_size>\n", row[6]);
		fprintf(xml, "\t<short_description><![CDATA[%s]]></short_description>\n", row[7]);
		fprintf(xml, "\t<description><![CDATA[%s]]></description>\n", row[8]);
		if (row[9] && cutSpaces(row[9])!="") fprintf(xml, "\t<changelog><![CDATA[%s]]></changelog>\n", row[9]);
		fprintf(xml, "\t<md5>%s</md5>\n", row[12]);
		fprintf(xml, "\t<maintainer>\n");
		fprintf(xml, "\t\t<name>%s</name>\n", row[10]);
		fprintf(xml, "\t\t<email>%s</email>\n", row[11]);
		fprintf(xml, "\t</maintainer>\n");

		fprintf(xml, "\t<location>%s</location>\n", getLocation(drepo, darch, dbranch, row[18], row[19], row[20], row[17]).c_str());

		fprintf(xml, "\t<filename>%s</filename>\n", row[13]);

		// Deps... :)
		res = mysql_query(&conn, string("SELECT dependency_package_name, dependency_condition, dependency_package_version FROM dependencies WHERE packages_package_id='" + string(row[0]) + "'").c_str());
		if (res) report_failure(__LINE__, mysql_error(&conn));
		deps = mysql_store_result(&conn);
		if (mysql_num_rows(deps)>0) {
			fprintf(xml, "\t<dependencies>\n");
			while(drow = mysql_fetch_row(deps)) {
				fprintf(xml, "\t\t<dep>\n");
				fprintf(xml, "\t\t\t<name>%s</name>\n", drow[0]);
				fprintf(xml, "\t\t\t<condition>%s</condition>\n", getDepConditionBack(atoi(drow[1])).c_str());
				fprintf(xml, "\t\t\t<version>%s</version>\n", drow[2]);
				fprintf(xml, "\t\t</dep>\n");
			}
			fprintf(xml, "\t</dependencies>\n");

		}

		mysql_free_result(deps);
		// Config files
		mysql_query(&conn, string("SELECT config_files.id, config_files.filename FROM config_files WHERE package_id='" + string(row[0]) + "'").c_str());
		confs = mysql_store_result(&conn);
		if (mysql_num_rows(confs)>0) {
			fprintf(xml, "\t<config_files>\n");
			while (crow = mysql_fetch_row(confs)) {
				attr.clear();
				mysql_query(&conn, string("SELECT name, value FROM config_options WHERE config_files_id='" + string(crow[0]) + "';").c_str());
				coptres = mysql_store_result(&conn);
				if (mysql_num_rows(coptres)>0) {
					while(coptrow = mysql_fetch_row(coptres)) {
						attr += " " + string(coptrow[0]) + "=\"" + string(coptrow[1]) + "\"";
					}
				}
				mysql_free_result(coptres);

				fprintf(xml, "\t\t<conf_file%s>%s</conf_file>\n", attr.c_str(), crow[1]);
			}
			fprintf(xml, "\t</config_files>\n");
		}
		mysql_free_result(confs);

		// Tags
		res = mysql_query(&conn, string("SELECT tags_name FROM tags,tags_links WHERE tags.tags_id=tags_links.tags_tag_id AND tags_links.packages_package_id='" + string(row[0]) + "'").c_str());
		if (res) report_failure(__LINE__);
		tags = mysql_store_result(&conn);
		if (tags && mysql_num_rows(tags)>0) {
			fprintf(xml, "\t<tags>\n");
			while (trow=mysql_fetch_row(tags)) {
				fprintf(xml, "\t\t<tag>%s</tag>\n", trow[0]);
			}
			fprintf(xml, "\t</tags>\n");
		}
		mysql_free_result(tags);

		fprintf(xml, "\t<repository_tags>%s</repository_tags>\n", row[14]);
		fprintf(xml, "\t<distro_version>%s</distro_version>\n", fancydistro.c_str());

		// ABUILD, if exist
		mysql_data_seek(abuilds, 0);
		while (abuild = mysql_fetch_row(abuilds)) {
			if (abuild[0]==NULL) continue;
			if (atoi(abuild[0])!=atoi(row[0])) continue;

			if (!abuild[1]) continue;
			fprintf(xml, "\t<abuild>%s</abuild>\n", getLocation(drepo, darch, dbranch, row[18], row[19], row[20], abuild[1]).c_str());

			break;
		}
		fprintf(xml, "</package>\n\n");
	}

	mysql_free_result(packages);
	mysql_free_result(abuilds);

	fprintf(xml, "</repository>\n");
	fclose(xml);
	fclose(package_list);


	printf("\tXML complete, compressing...\n");
	//system("( cat " + index_path + "/packages.xml | gzip -9 > " + index_path + "/packages.xml.gz.tmp && mv " + index_path + "/packages.xml.gz.tmp " + index_path + "/packages.xml.gz ) &"); // gzip indexes are deprecated from now.
	system("( cat " + index_path + "/packages.xml | xz -c > " + index_path + "/packages.xml.xz.tmp && mv " + index_path + "/packages.xml.xz.tmp " + index_path + "/packages.xml.xz ) &");


	// Also, we need setup variants index here.
	FILE *setup_variants;
	setup_variants=fopen(string(index_path + "/setup_variants.list").c_str(), "w");
	if (!setup_variants) {
		perror("Failed to open setup_variants file for writing");
		abort();
	}
	if (FileExists(index_path + "/setup_variants")) {
		vector<string> svar_raw = getDirectoryList(index_path + "/setup_variants");
		if (svar_raw.size()>0) {
			for (size_t i=0; i<svar_raw.size(); ++i) {
				if (svar_raw[i].find(".list")==std::string::npos) continue; // Skip files that are not lists
				if (svar_raw[i].find(".")==0) continue; // Skip hidden files
				if (svar_raw[i].find(".list")==svar_raw[i].size()-strlen(".list")) {
					fprintf(setup_variants, "%s\n", svar_raw[i].substr(0, svar_raw[i].size()-strlen(".list")).c_str());
				}
			}
		}
		system("( cd " + index_path + " && tar -cJvf setup_variants.tar.xz --exclude .git setup_variants )");
	}
	fclose(setup_variants);



	printf("Index generation completed.\n");

}
int main(int argc, char **argv) {
	MYSQL conn;
	if (!mysql_init(&conn)) {
		fprintf(stderr, "Omg, failed to init MYSQL\n");
		exit(1);
	}
	vector<string> db_config = ReadFileStrings("db.conf");
	if (db_config.size()<4) {
		fprintf(stderr, "Invalid configuration file. Should be:\nhostname\nusername\npassword\ndbname\n");
		exit(3);
	}
	if(!mysql_real_connect(&conn, db_config[0].c_str(), db_config[1].c_str(), db_config[2].c_str(), db_config[3].c_str(), 0, NULL, 0)) {
		fprintf(stderr, "Omg, failed to connect\n");
		exit(2);

	}
	mysql_set_character_set(&conn, "utf8");



	MYSQL_RES *available_repos;
	//int res = mysql_query(&conn, "SELECT server_url, distro_arch, distro_version FROM locations GROUP BY server_url, distro_arch");
	int res = mysql_query(&conn, "SELECT repositories.name, architectures.name, branches.name FROM repositories, branches, architectures");
	if (res) report_failure(__LINE__);
	available_repos = mysql_store_result(&conn);
	int repocount = mysql_num_rows(available_repos);
	printf("Total repos: %d\n", repocount);

	vector<string> drepo, darch, dbranch, tmp_branch, tmp_arch, tmp_repo;

	MYSQL_ROW row;
	bool found;
	while (row = mysql_fetch_row(available_repos)) {
		found = false;
		for (size_t i=0; !found && i<drepo.size(); ++i) if (drepo[i]==cutSpaces(row[0])) found = true;
		if (!found) drepo.push_back(cutSpaces(row[0]));

		found = false;
		for (size_t i=0; !found && i<darch.size(); ++i) if (darch[i]==cutSpaces(row[1])) found = true;
		if (!found) darch.push_back(cutSpaces(row[1]));

		found = false;
		for (size_t i=0; !found && i<dbranch.size(); ++i) if (dbranch[i]==cutSpaces(row[2])) found = true;
		if (!found) {
			dbranch.push_back(cutSpaces(row[2]));
			dbranch.push_back(cutSpaces(row[2]) + "_deprecated");
		}
	}

	// So, we need indexes: 
	// 	/$branch/$repo/$arch/repository/
	// 	/$branch/$repo/
	// 	/$branch/
	// 	/

	// Global index: /
	generateIndex2(conn, tmp_repo, tmp_arch, tmp_branch);
	// Deprecated branch:
	tmp_branch.push_back("8.0_deprecated");
	generateIndex2(conn, tmp_repo, tmp_arch, tmp_branch, "deprecated_package_tree", true);

	for (size_t i=0; i<drepo.size(); ++i) {
		// Index: /$repo/
		tmp_branch.clear();
		tmp_arch.clear();
		tmp_repo.clear();

		tmp_repo.push_back(drepo[i]);
		generateIndex2(conn, tmp_repo, tmp_arch, tmp_branch);
		for (size_t t=0; t<dbranch.size(); ++t) {
			// Index: /$repo/$branch/
			tmp_branch.clear();
			tmp_arch.clear();
			tmp_branch.push_back(dbranch[t]);
			generateIndex2(conn, tmp_repo, tmp_arch, tmp_branch);
			for (size_t z=0; z<darch.size(); ++z) {
				// Index: /$repo/$branch/$arch/repository/
				tmp_arch.clear();
				tmp_arch.push_back(darch[z]);
				generateIndex2(conn, tmp_repo, tmp_arch, tmp_branch);
			}
		}
	}


	printf("Index generation finished, total: %d indexes created\n", index_counter);
}

